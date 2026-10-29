#include "worker.h"
#include "../archivenames.h"
#include <QFileInfo>
#include <clocale>
#include <csignal>
#include <unistd.h>

using namespace nook::archive;
using namespace nook::archive::worker;

int main() {
    // Leading its own process group lets the client stop it together with any 7-Zip it started.
    ::setpgid(0, 0);
    ::signal(SIGPIPE, SIG_IGN);
    // Names go to and from the file system as UTF-8; libarchive hands over legacy names as raw bytes.
    if (!std::setlocale(LC_CTYPE, "C.UTF-8"))
        std::setlocale(LC_CTYPE, "C.utf8");
    currentUmask();

    auto &channel = Channel::instance();
    Request request;
    request.json = channel.readRequest();
    request.op = request.json.value("op").toString();
    const QString archive = request.json.value("archive").toString();
    request.archive = archive.isEmpty() ? QString() : QFileInfo(archive).absoluteFilePath();
    request.json.insert("archive", request.archive);
    request.hasPassword = request.json.contains("password");
    request.password = request.json.value("password").toString();
    request.encoding = request.json.value("encoding").toString("auto");
    if (request.encoding != "auto" && !filenameEncodings().contains(request.encoding))
        request.encoding = "auto";
    channel.listen();

    QString writable;
    if (request.op == "extract")
        writable = QFileInfo(destinationOf(request.json)).absoluteFilePath();
    else if (request.op == "create")
        writable = QFileInfo(request.json.value("output").toString()).absolutePath();
    const bool sandboxed = !qEnvironmentVariableIsSet("NOOK_ARCHIVE_NO_SANDBOX") && restrictWrites(writable);
    channel.send({{"event", "started"}, {"sandbox", sandboxed}, {"pid", int(::getpid())}});

    QJsonObject done;
    if (request.op == "create") {
        done = create(request);
    } else if (request.op == "list" || request.op == "test" || request.op == "extract") {
        if (request.archive.isEmpty() || !QFileInfo::exists(volumesOf(request.archive).first)) {
            const bool split = volumesOf(request.archive).split;
            done = {{"event", "done"}, {"ok", false},
                    {"error", split ? failure("missing-volume", "找不到第一个分卷")
                                    : failure("not-found", "找不到压缩包")}};
        } else {
            bool fallback = false;
            if (backendFor(request.archive) == Backend::Libarchive) {
                done = request.op == "list"   ? libarchiveList(request, &fallback)
                       : request.op == "test" ? libarchiveTest(request, &fallback)
                                              : libarchiveExtract(request, &fallback);
            } else {
                fallback = true;
            }
            if (fallback) {
                done = request.op == "list"   ? sevenZipList(request)
                       : request.op == "test" ? sevenZipTest(request)
                                              : sevenZipExtract(request);
            }
        }
    } else {
        done = {{"event", "done"}, {"ok", false}, {"error", failure("request", "未知的请求：" + request.op)}};
    }
    if (done.isEmpty())
        done = {{"event", "done"}, {"ok", false}, {"error", failure("internal", "内部错误")}};
    channel.send(done);
    std::fflush(stdout);
    // The stdin reader thread may still be blocked; leave without unwinding it.
    ::_exit(0);
}
