#pragma once

// C++ includes
#include <string>

// Euclid includes
#include <euclid/core/monitoring/MetricEventBus.h>

namespace Euclid::Transfer {

    /**
     * @brief Metrics every transfer server process records, labelled by the server it runs.
     *
     * @par
     * FTP and SFTP report the same four names: what a client uploaded and downloaded is the same
     * measurement whichever protocol carried it, and the protocol is already recorded on the ETS
     * server definition the label points at. One transfer server is one process, so the
     * monitoring module sums these per label across however many processes are up.
     *
     * @par Byte counts
     * Counted at the protocol, not at the bucket: bytes received are what the client actually
     * sent, bytes sent are what went back out to it. For FTP those are the same as the object's
     * size, but an SFTP client may read a file in pieces, re-read part of it, or write into the
     * middle of one - the wire is the honest measure there.
     *
     * @par File counts
     * Counted at the bucket: one received file is one object stored (FTP STOR, SFTP CLOSE of a
     * written handle), one sent file is one object fetched out of the bucket for a client (FTP
     * RETR, SFTP OPEN for reading). Sending the same file twice counts twice.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    namespace Metrics {

        constexpr auto kLabel = "server";
        constexpr auto kBytesReceived = "ets-bytes-received";
        constexpr auto kBytesSent = "ets-bytes-sent";
        constexpr auto kFilesReceived = "ets-files-received";
        constexpr auto kFilesSent = "ets-files-sent";

        /**
         * @brief Records bytes a client uploaded to this server.
         */
        inline void BytesReceived(const std::string &serverId, const long bytes) {
            if (bytes <= 0) return;
            Core::Monitoring::MetricEventBus::instance().sigMetricCounter(kBytesReceived, kLabel, serverId, static_cast<double>(bytes));
        }

        /**
         * @brief Records bytes this server sent to a client.
         */
        inline void BytesSent(const std::string &serverId, const long bytes) {
            if (bytes <= 0) return;
            Core::Monitoring::MetricEventBus::instance().sigMetricCounter(kBytesSent, kLabel, serverId, static_cast<double>(bytes));
        }

        /**
         * @brief Records one file stored in this server's bucket.
         */
        inline void FileReceived(const std::string &serverId) {
            Core::Monitoring::MetricEventBus::instance().sigMetricRate(kFilesReceived, kLabel, serverId);
        }

        /**
         * @brief Records one file fetched out of this server's bucket for a client.
         */
        inline void FileSent(const std::string &serverId) {
            Core::Monitoring::MetricEventBus::instance().sigMetricRate(kFilesSent, kLabel, serverId);
        }

    }// namespace Metrics

}// namespace Euclid::Transfer
