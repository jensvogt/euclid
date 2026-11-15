# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

"""What setup-infrastructure.sh sets up, drawn with the AWS architecture icons.

euclid's modules have AWS counterparts, and their icons stand in for them here:
ETS = Transfer Family, ESM = S3, EQS = SQS, ENS = SNS, EAP applications = ECS containers,
EAM = IAM, and euclid's log = CloudWatch Logs.

Renders setup-infrastructure.png next to this file. It needs Graphviz and the `diagrams` package;
render.sh runs it in a container, so neither has to be installed.
"""

from diagrams import Cluster, Diagram, Edge
from diagrams.aws.compute import ElasticContainerServiceContainer
from diagrams.aws.integration import SimpleNotificationServiceSnsTopic, SimpleQueueServiceSqsQueue
from diagrams.aws.management import CloudwatchLogs
from diagrams.aws.migration import TransferForSftp
from diagrams.aws.security import IAMRole
from diagrams.aws.storage import SimpleStorageServiceS3Bucket
from diagrams.aws.general import Users

GRAPH = {
    "fontsize": "18",
    "labelloc": "t",
    "pad": "0.3",
    "nodesep": "0.35",
    "ranksep": "0.7",
    # Curved edges: with the default right-angled ones, Graphviz places edge labels away from
    # their edges.
    "splines": "spline",
}
NODE = {"fontsize": "11"}
EDGE = {"fontsize": "10"}

# Dashed: deploys or permits rather than carries data.
deploys = {"style": "dashed", "color": "gray40"}

with Diagram(
    "euclid examples/docker - setup-infrastructure.sh",
    filename="setup-infrastructure",
    outformat="png",
    direction="LR",
    show=False,
    graph_attr=GRAPH,
    node_attr=NODE,
    edge_attr=EDGE,
):
    users = Users("ftpuser1, ftpuser2\ngroup ftp-users")

    with Cluster("ETS - transfer servers"):
        ftp = TransferForSftp("ftp-server\n:2121")
        sftp = TransferForSftp("sftp-server\n:2222")

    with Cluster("ESM - buckets"):
        transfer = SimpleStorageServiceS3Bucket("transfer-server\n<user>/incoming/...")
        delivered = SimpleStorageServiceS3Bucket("file-delivery\n<user>/...")
        apps = SimpleStorageServiceS3Bucket("apps\nartifacts")

    with Cluster("EAP - applications on wrk-1, wrk-2"):
        file_delivery = ElasticContainerServiceContainer("file-delivery\nC++, 1-4 instances")
        printer = ElasticContainerServiceContainer("protocol-printer\nRust, 1 instance")
        splitter = ElasticContainerServiceContainer("xml-splitter\nPython, 1 instance")

    with Cluster("EQS - queues"):
        uploads = SimpleQueueServiceSqsQueue("file-delivery-queue")
        # Four queues, one node: an edge to each would say the same thing four times. The XML one
        # on its own, since an application reads it.
        classified = SimpleQueueServiceSqsQueue("file-delivery-\n{pdf,image,json,misc}\nby content (libmagic)")
        xml_queue = SimpleQueueServiceSqsQueue("file-delivery-xml")
        pieces = SimpleQueueServiceSqsQueue("splitting-output\none message per CD")
        printer_queue = SimpleQueueServiceSqsQueue("protocol-printer-queue")

    with Cluster("ENS - topics"):
        protocol = SimpleNotificationServiceSnsTopic("protocol\none message per step")

    log = CloudwatchLogs("euclid log")

    # Who may do what, as a note rather than edges: lines from it to every principal only add
    # crossings.
    with Cluster("EAM - roles"):
        IAMRole("transfer -> ftp-users\nfile-delivery, -cleanup, -protocol\nprotocol-printer\nxml-splitter")

    # The upload path.
    users >> Edge(label="FTP / SFTP") >> [ftp, sftp]
    ftp >> transfer
    sftp >> transfer
    transfer >> Edge(label="object created / updated") >> uploads
    uploads >> file_delivery

    # What file-delivery does with each upload.
    file_delivery >> Edge(label="deliver (ZIPs unpacked)") >> delivered
    file_delivery >> Edge(label="delete when delivered", style="dashed", color="firebrick") >> transfer
    file_delivery >> [classified, xml_queue]
    # Unlabelled: the topic's own label says what goes there, and a label on this long edge lands
    # beside whichever node it passes.
    file_delivery >> protocol

    # And the protocol, printed.
    protocol >> printer_queue >> printer
    printer >> Edge(label="stdout") >> log

    # And the XML files, split.
    xml_queue >> splitter
    delivered >> Edge(label="download XML") >> splitter
    splitter >> Edge(label="split per CD") >> pieces

    # Deployment.
    apps >> Edge(label="artifact", **deploys) >> [file_delivery, printer, splitter]
