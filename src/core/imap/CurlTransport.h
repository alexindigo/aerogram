#ifndef CURLTRANSPORT_H
#define CURLTRANSPORT_H

#include <curl/curl.h>

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

/// \brief Minimal libcurl IMAP transport. Blocking; intended for use
///        inside QtConcurrent workers, never on the UI thread.
///
/// Uses explicit CUSTOMREQUESTs so behavior is deterministic:
///   listFolders    -> LIST "" *
///   uidSearchAll   -> UID SEARCH ALL
///   fetchMessage   -> UID FETCH <uid> (FLAGS BODY.PEEK[])  (no \Seen side
///                       effect — plain BODY[] implicitly sets \Seen per
///                       RFC 3501 §6.4.5)
///
/// IMAP response framing is stripped by parsing the {N} literal marker.
class CurlTransport
{
public:
    CurlTransport(QString host, int port, QString user, QString pass, bool tls)
        : m_host(std::move(host))
        , m_port(port)
        , m_user(std::move(user))
        , m_pass(std::move(pass))
        , m_tls(tls)
    {
    }

    bool listFolders(QStringList &folders, QString *err)
    {
        QByteArray resp;
        if (!perform(QString(), QStringLiteral("LIST \"\" *"), resp, err))
            return false;

        const auto lines = resp.split('\n');
        for (const QByteArray &rawLine : lines) {
            const QByteArray line = rawLine.trimmed();
            if (!line.startsWith("* LIST")) continue;

            // Format: * LIST (<flags>) "<delimiter>" <mailbox>
            // Mailbox name is the last token, possibly quoted.
            QByteArray name = line.mid(line.lastIndexOf(' ') + 1);
            if (name.startsWith('"') && name.endsWith('"') && name.size() >= 2)
                name = name.mid(1, name.size() - 2);
            if (!name.isEmpty())
                folders.append(QString::fromUtf8(name));
        }
        folders.removeDuplicates();
        return true;
    }

    bool uidSearchAll(const QString &folder, QList<int> &uids, QString *err)
    {
        QByteArray resp;
        if (!perform(folder, QStringLiteral("UID SEARCH ALL"), resp, err))
            return false;

        const auto lines = resp.split('\n');
        for (const QByteArray &rawLine : lines) {
            const QByteArray line = rawLine.trimmed();
            if (!line.startsWith("* SEARCH")) continue;
            const auto tokens = line.mid(8).trimmed().split(' ');
            for (const QByteArray &tok : tokens) {
                bool ok = false;
                const int uid = tok.toInt(&ok);
                if (ok) uids.append(uid);
            }
        }
        return true;
    }

    bool fetchMessage(const QString &folder, int uid, QByteArray &rawOut,
                      bool &seenOut, QString *err)
    {
        QByteArray resp;
        const QString req = QStringLiteral("UID FETCH %1 (FLAGS BODY.PEEK[])").arg(uid);
        if (!perform(folder, req, resp, err))
            return false;

        const int open = resp.indexOf('{');
        if (open < 0) {
            if (err) *err = QStringLiteral("No literal in FETCH response for UID %1").arg(uid);
            return false;
        }

        // Flags live in the pre-literal region.
        const QByteArray pre = resp.left(open);
        seenOut = pre.contains("\\Seen");

        const int close = resp.indexOf('}', open);
        if (close < 0) {
            if (err) *err = QStringLiteral("Malformed literal marker for UID %1").arg(uid);
            return false;
        }
        bool ok = false;
        const int len = resp.mid(open + 1, close - open - 1).toInt(&ok);
        if (!ok) {
            if (err) *err = QStringLiteral("Bad literal length for UID %1").arg(uid);
            return false;
        }

        int start = close + 1;
        if (resp.mid(start, 2) == "\r\n") start += 2;
        else if (resp.mid(start, 1) == "\n") start += 1;

        if (resp.size() < start + len) {
            if (err) *err = QStringLiteral("Truncated literal for UID %1").arg(uid);
            return false;
        }

        rawOut = resp.mid(start, len);
        return true;
    }

private:
    static size_t writeCb(char *ptr, size_t size, size_t nmemb, void *userdata)
    {
        auto *buf = static_cast<QByteArray *>(userdata);
        buf->append(ptr, static_cast<qsizetype>(size * nmemb));
        return size * nmemb;
    }

    bool perform(const QString &folder, const QString &request,
                 QByteArray &respOut, QString *err)
    {
        CURL *curl = curl_easy_init();
        if (!curl) {
            if (err) *err = QStringLiteral("curl_easy_init failed");
            return false;
        }

        QByteArray url = (m_tls ? "imaps://" : "imap://")
                       + m_host.toUtf8() + ":" + QByteArray::number(m_port) + "/";
        if (!folder.isEmpty()) {
            char *esc = curl_easy_escape(curl, folder.toUtf8().constData(),
                                         static_cast<int>(folder.toUtf8().size()));
            url += esc;
            curl_free(esc);
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.constData());
        curl_easy_setopt(curl, CURLOPT_USERNAME, m_user.toUtf8().constData());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, m_pass.toUtf8().constData());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &CurlTransport::writeCb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &respOut);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
        if (!request.isEmpty())
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.toUtf8().constData());

        const CURLcode rc = curl_easy_perform(curl);
        if (rc != CURLE_OK) {
            if (err) *err = QString::fromUtf8(curl_easy_strerror(rc));
            curl_easy_cleanup(curl);
            return false;
        }

        curl_easy_cleanup(curl);
        return true;
    }

    QString m_host;
    int m_port;
    QString m_user;
    QString m_pass;
    bool m_tls;
};

#endif
