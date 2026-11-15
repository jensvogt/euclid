// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! protocol-printer - a euclid application in Rust, written against euclid-rdk.
//!
//! Prints every message published to the ENS topic "protocol" to stdout, one per line, exactly as
//! it was published. The file-delivery example publishes one JSON message there for every step it
//! takes, so this is that application's protocol as a stream:
//!
//! ```text
//! {"timestamp":"2026-11-15T14:02:11.482Z","application":"file-delivery","step":"picked-up",...}
//! {"timestamp":"2026-11-15T14:02:11.613Z","application":"file-delivery","step":"downloaded",...}
//! ```
//!
//! A topic is not read directly: its messages are delivered to the queues subscribed to it. So on
//! startup this makes a queue of its own, subscribes it to the topic, and from then on long-polls
//! that queue - printing each message, then deleting it.
//!
//! It runs two ways:
//!
//! - deployed as an application (runtime RUST), with the credentials the manager hands it in
//!   EUCLID_CREDENTIALS_FILE - stdout then lands in euclid's log under the application's channel;
//! - on a developer's machine, logging in with EUCLID_USER and EUCLID_PASSWORD - stdout is the
//!   terminal, or `| jq`.

use std::error::Error;
use std::io::Write;
use std::path::PathBuf;
use std::time::Duration;

use euclid_rdk::credentials::CachedCredentials;
use euclid_rdk::eam::Eam;
use euclid_rdk::{Ens, Eqs};
use serde::Deserialize;

/// How long a reception may wait for messages. Twenty seconds is EQS's maximum, and a long poll is
/// how a consumer stays responsive without spinning.
const WAIT_SECONDS: u64 = 20;

/// How many messages one receive may take.
const BATCH_SIZE: u64 = 10;

type AnyResult<T> = Result<T, Box<dyn Error + Send + Sync>>;

// -- configuration ------------------------------------------------------------------------------

struct Config {
    /// The topic whose messages are printed.
    topic: String,
    /// The queue subscribed to it. Instances of one application share it, so each message is
    /// printed once, by one of them; a run on a developer's machine uses one of its own, so it
    /// does not take messages away from a deployed instance.
    queue: String,
    /// Where the credentials come from.
    source: Source,
}

enum Source {
    /// Deployed: the file the manager writes, and replaces before the token in it runs out.
    Application { credentials_file: PathBuf, ca_cert: Option<PathBuf> },
    /// On a developer's machine: a login of its own, or a session euclid-rdk cached earlier.
    Local {
        url: String,
        ca_cert: Option<PathBuf>,
        user: Option<String>,
        password: Option<String>,
        namespace: Option<String>,
    },
}

fn env(name: &str) -> Option<String> {
    std::env::var(name).ok().filter(|value| !value.is_empty())
}

fn load_config() -> Config {
    let application_id = env("EUCLID_APPLICATION_ID");
    let queue_owner = application_id.clone().unwrap_or_else(|| "protocol-printer-local".into());
    let source = match env("EUCLID_CREDENTIALS_FILE") {
        Some(file) => Source::Application {
            credentials_file: PathBuf::from(file),
            ca_cert: env("EUCLID_CA_CERT_PATH").map(PathBuf::from),
        },
        None => Source::Local {
            url: env("EUCLID_URL").unwrap_or_else(|| "https://localhost:5566".into()),
            ca_cert: env("EUCLID_CA_CERT_PATH").map(PathBuf::from),
            user: env("EUCLID_USER"),
            password: env("EUCLID_PASSWORD"),
            namespace: env("EUCLID_NAMESPACE"),
        },
    };
    Config {
        topic: env("PROTOCOL_TOPIC").unwrap_or_else(|| "protocol".into()),
        queue: env("PROTOCOL_QUEUE").unwrap_or_else(|| format!("{queue_owner}-queue")),
        source,
    }
}

// -- the session --------------------------------------------------------------------------------

/// The credentials file the manager writes for an application (see
/// examples/applications/README.md). Only what a session needs is read.
#[derive(Deserialize, Default)]
#[serde(rename_all = "camelCase", default)]
struct ApplicationCredentials {
    token: String,
    user_id: Option<String>,
    account_id: Option<String>,
    region: Option<String>,
    namespace: Option<String>,
    endpoint: String,
}

/// The clients, and the token they were made with.
struct Clients {
    token: String,
    ens: Ens,
    eqs: Eqs,
}

/// Opens a session from wherever the credentials come from.
///
/// An application's credentials file is not the CLI's: it names the server "endpoint" rather than
/// "baseUrl", and euclid-rdk only reuses a cached token whose file names the server it is asked
/// for. So the token is copied into a file of the CLI's shape, private to this process, and the
/// session opened from that - no password, and nothing sent to the server to open it.
async fn connect(source: &Source) -> AnyResult<Clients> {
    match source {
        Source::Application { credentials_file, ca_cert } => {
            let app = read_application_credentials(credentials_file)?;
            let cache = std::env::temp_dir().join(format!("protocol-printer-{}.json", std::process::id()));
            euclid_rdk::credentials::store(
                &cache,
                &CachedCredentials {
                    token: Some(app.token.clone()),
                    user_id: app.user_id,
                    account_id: app.account_id,
                    region: app.region,
                    namespace: app.namespace,
                    base_url: Some(app.endpoint.clone()),
                    ..Default::default()
                },
            )?;
            let mut access = Eam::for_server(app.endpoint).credentials_file(&cache);
            if let Some(ca_cert) = ca_cert {
                access = access.ca_cert_path(ca_cert);
            }
            let session = access.login().await?;
            Ok(Clients { token: app.token, ens: session.ens(), eqs: session.eqs() })
        }
        Source::Local { url, ca_cert, user, password, namespace } => {
            let mut access = Eam::for_server(url);
            if let Some(ca_cert) = ca_cert {
                access = access.ca_cert_path(ca_cert);
            }
            // Topics and queues live in a namespace, and a login lands in the user's default one.
            if let Some(namespace) = namespace {
                access = access.namespace(namespace);
            }
            // With a user and password, a login of its own that is not cached: the cache is the
            // file euclid-cli keeps its session in, and this is no reason to replace that. Without
            // them, a session euclid-rdk or euclid-jdk cached for this server - euclid-cli's own
            // does not do, as it does not record which server it is for.
            if let (Some(user), Some(password)) = (user, password) {
                access = access.credentials(user, password).no_cache();
            }
            let session = access.login().await?;
            Ok(Clients { token: session.token().to_owned(), ens: session.ens(), eqs: session.eqs() })
        }
    }
}

fn read_application_credentials(path: &PathBuf) -> AnyResult<ApplicationCredentials> {
    let text = std::fs::read_to_string(path).map_err(|e| format!("cannot read {}: {e}", path.display()))?;
    let credentials: ApplicationCredentials = serde_json::from_str(&text)?;
    if credentials.token.is_empty() || credentials.endpoint.is_empty() {
        return Err(format!("{} names no token or no endpoint", path.display()).into());
    }
    Ok(credentials)
}

/// Whether the manager has put a new token in the file since `clients` were made. It does so
/// halfway through the old one's life, and a client keeps presenting the token it was made with -
/// so without this the application works for its first token's lifetime and then gets 401s.
fn token_rotated(source: &Source, clients: &Clients) -> bool {
    match source {
        Source::Application { credentials_file, .. } => read_application_credentials(credentials_file)
            .map(|current| current.token != clients.token)
            .unwrap_or(false),
        Source::Local { .. } => false,
    }
}

// -- startup ------------------------------------------------------------------------------------

async fn ensure_queue(eqs: &Eqs, name: &str) -> AnyResult<String> {
    // Two calls rather than a create that tolerates failure: "already exists" and "you may not" are
    // different answers, and only one of them is fine to carry on from.
    let queue = if eqs.exists_queue(name).await? {
        eqs.get_queue_ern(name).await?
    } else {
        eprintln!("creating queue {name}");
        eqs.create_queue(name).await?
    };
    queue.ern.ok_or_else(|| format!("queue {name} has no ERN").into())
}

/// Subscribes the queue to the topic, unless it already is. A second subscription would deliver
/// every message twice, so a restart checks first.
async fn ensure_subscription(ens: &Ens, topic_ern: &str, queue_ern: &str) -> AnyResult<()> {
    let subscriptions = ens.list_subscriptions(topic_ern).await?.subscriptions;
    if subscriptions.iter().any(|s| s.target_ern.as_deref() == Some(queue_ern)) {
        return Ok(());
    }
    let subscription = ens.subscribe(topic_ern, queue_ern).await?;
    eprintln!("subscribed the queue to the topic, subscription: {}", subscription.ern.unwrap_or_default());
    Ok(())
}

// -- shutdown -----------------------------------------------------------------------------------

/// Resolves on SIGTERM - what the manager sends to stop an instance - or Ctrl-C.
async fn shutdown_signal() {
    #[cfg(unix)]
    {
        let mut terminate = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())
            .expect("cannot install the SIGTERM handler");
        tokio::select! {
            _ = terminate.recv() => {}
            _ = tokio::signal::ctrl_c() => {}
        }
    }
    #[cfg(not(unix))]
    {
        let _ = tokio::signal::ctrl_c().await;
    }
}

// -- main ---------------------------------------------------------------------------------------

#[tokio::main]
async fn main() {
    if let Err(e) = run().await {
        eprintln!("protocol-printer: {e}");
        std::process::exit(1);
    }
}

async fn run() -> AnyResult<()> {
    let config = load_config();

    // To stderr, like every other line of its own: stdout carries the messages and nothing else,
    // so it can be piped into jq.
    eprintln!(
        "protocol-printer starting, version: {}, topic: {}, queue: {}",
        env("EUCLID_APPLICATION_VERSION").unwrap_or_else(|| "(unset)".into()),
        config.topic,
        config.queue
    );

    let mut clients = connect(&config.source).await?;

    // The topic has to exist already: it is the installation's (setup-infrastructure.sh). The
    // queue is this application's own, and made here if it is not there.
    let topic_ern = clients
        .ens
        .get_topic_ern(&config.topic)
        .await
        .map_err(|e| format!("no topic {}: {e}", config.topic))?
        .ern
        .ok_or("the topic has no ERN")?;
    let queue_ern = ensure_queue(&clients.eqs, &config.queue).await?;
    ensure_subscription(&clients.ens, &topic_ern, &queue_ern).await?;

    let shutdown = shutdown_signal();
    tokio::pin!(shutdown);
    let mut printed: u64 = 0;

    loop {
        if token_rotated(&config.source, &clients) {
            match connect(&config.source).await {
                Ok(fresh) => clients = fresh,
                Err(e) => eprintln!("cannot renew the session, keeping the old one: {e}"),
            }
        }

        // Only the wait is cancelled by a stop, never the printing below: a receive abandoned
        // halfway leaves its messages to come back after their visibility timeout.
        let batch = tokio::select! {
            _ = &mut shutdown => break,
            batch = clients.eqs.receive_messages(&queue_ern, BATCH_SIZE, WAIT_SECONDS) => batch,
        };

        let messages = match batch {
            Ok(batch) => batch.messages,
            Err(e) => {
                // A queue that cannot be reached is worth a line and another try, not an exit.
                eprintln!("receive failed: {e}");
                tokio::time::sleep(Duration::from_secs(1)).await;
                continue;
            }
        };

        let mut stdout = std::io::stdout().lock();
        for message in &messages {
            // Printed, flushed, and only then deleted: a crash in between prints a message twice
            // rather than never.
            writeln!(stdout, "{}", message.body.as_deref().unwrap_or_default())?;
            stdout.flush()?;
            printed += 1;
            if let Some(receipt) = &message.receipt_handle {
                if let Err(e) = clients.eqs.delete_message(receipt).await {
                    eprintln!("delete failed, the message will be printed again: {e}");
                }
            }
        }
    }

    eprintln!("protocol-printer stopping, messages printed: {printed}");
    Ok(())
}
