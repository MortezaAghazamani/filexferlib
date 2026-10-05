#include "filexferlib/client.hpp"
#include "filexferlib/detail/protocol.hpp"

#include <filesystem>
#include <map>
#include <vector>
#include <algorithm>
#include <thread>
#include <cstdlib>

namespace fs = std::filesystem;

namespace filexferlib {

namespace {

// =====================================================================
// scan_local — recursive walk of a local directory, files only
// =====================================================================

std::map<std::string, FileMeta> scan_local(const fs::path& root,
                                           const SyncOptions& opts,
                                           std::error_code& ec) {
    std::map<std::string, FileMeta> out;
    if (!fs::exists(root, ec)) return out;

    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;

        const auto& entry = *it;

        // Skip directories and non-regular files. Directories are
        // created automatically by the receiver when a file inside
        // them is transferred.
        std::error_code dEc;
        if (entry.is_directory(dEc)) continue;
        if (!entry.is_regular_file(dEc)) continue;

        std::error_code relEc;
        const auto rel = fs::relative(entry.path(), root, relEc);
        if (relEc) continue;

        std::string relPosix = rel.generic_string();
        if (relPosix.empty()) continue;
        if (relPosix[0] != '/') relPosix = "/" + relPosix;

        if (opts.filter && !opts.filter(relPosix)) continue;

        FileMeta m;
        m.relativePath = relPosix;
        m.isDirectory = false;

        std::error_code sEc;
        m.size = fs::file_size(entry.path(), sEc);
        if (sEc) continue;

        std::error_code tEc;
        auto t = fs::last_write_time(entry.path(), tEc);
        if (!tEc) {
            m.mtimeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                t.time_since_epoch()).count();
        }

        out[relPosix] = m;
    }
    return out;
}

} // namespace

// =====================================================================
// syncFolder
// =====================================================================

std::future<SyncResult> Client::syncFolder(
    const fs::path& localDir,
    const std::string& remoteDir,
    SyncOptions opts,
    SyncProgressCallback onProgress,
    CancellationToken cancel)
{
    auto pr = std::make_shared<std::promise<SyncResult>>();
    auto fut = pr->get_future();

    if (!session_) {
        pr->set_exception(std::make_exception_ptr(
            std::runtime_error("not connected")));
        return fut;
    }
    if (!cancel) cancel = make_cancellation_token();

    Client* self = this;

    std::thread([self, localDir, remoteDir, opts, onProgress, cancel, pr]() mutable {
        try {
            SyncResult result;
            std::error_code ec;

            // -----------------------------------------------------
            // 1. Scan local
            // -----------------------------------------------------
            auto localIndex = scan_local(localDir, opts, ec);
            if (ec) {
                throw std::system_error(ec, "local scan");
            }

            // -----------------------------------------------------
            // 2. Scan remote (files only)
            // -----------------------------------------------------
            std::map<std::string, FileMeta> remoteIndex;
            {
                auto remoteFut = self->listRemote(remoteDir);
                auto remoteList = remoteFut.get();
                for (auto& m : remoteList) {
                    if (m.isDirectory) continue;
                    remoteIndex[m.relativePath] = m;
                }
            }

            // -----------------------------------------------------
            // 3. Build plan
            // -----------------------------------------------------
            struct Op {
                enum Kind {
                    Upload,
                    Download,
                    DeleteLocal,
                    DeleteRemote,
                    Skip
                } kind;
                std::string rel;
                FileMeta local;
                FileMeta remote;
            };
            std::vector<Op> plan;

            auto consider = [&](const std::string& rel,
                                const FileMeta* l,
                                const FileMeta* r) {
                if (opts.filter && !opts.filter(rel)) return;

                // Safety: never process directories.
                if (l && l->isDirectory) return;
                if (r && r->isDirectory) return;

                if (l && !r) {
                    // File exists only locally
                    if (opts.mode == SyncMode::Upload ||
                        opts.mode == SyncMode::MirrorUpload ||
                        opts.mode == SyncMode::Bidirectional)
                    {
                        plan.push_back({Op::Upload, rel, *l, {}});
                    }
                    else if (opts.mode == SyncMode::MirrorDownload)
                    {
                        // MirrorDownload: local file not on remote → delete
                        plan.push_back({Op::DeleteLocal, rel, *l, {}});
                    }
                    else
                    {
                        plan.push_back({Op::Skip, rel, *l, {}});
                    }
                }
                else if (!l && r) {
                    // File exists only on remote
                    if (opts.mode == SyncMode::Download ||
                        opts.mode == SyncMode::MirrorDownload ||
                        opts.mode == SyncMode::Bidirectional)
                    {
                        plan.push_back({Op::Download, rel, {}, *r});
                    }
                    else if (opts.mode == SyncMode::MirrorUpload)
                    {
                        // MirrorUpload: remote file not on local → delete
                        plan.push_back({Op::DeleteRemote, rel, {}, *r});
                    }
                    else
                    {
                        plan.push_back({Op::Skip, rel, {}, *r});
                    }
                }
                else if (l && r) {
                    // File exists on both sides
                    bool same = (l->size == r->size);
                    if (same && opts.compareBy != CompareMethod::SizeOnly) {
                        const auto diff = std::llabs(l->mtimeNs - r->mtimeNs);
                        const auto tol = std::chrono::duration_cast<
                            std::chrono::nanoseconds>(opts.mtimeTolerance).count();
                        same = diff <= tol;
                    }
                    if (same) {
                        plan.push_back({Op::Skip, rel, *l, *r});
                        return;
                    }
                    if (opts.mode == SyncMode::Upload ||
                        opts.mode == SyncMode::MirrorUpload)
                    {
                        plan.push_back({Op::Upload, rel, *l, *r});
                    }
                    else if (opts.mode == SyncMode::Download ||
                             opts.mode == SyncMode::MirrorDownload)
                    {
                        plan.push_back({Op::Download, rel, *l, *r});
                    }
                    else
                    {
                        // Bidirectional — newer wins
                        if (l->mtimeNs >= r->mtimeNs)
                            plan.push_back({Op::Upload, rel, *l, *r});
                        else
                            plan.push_back({Op::Download, rel, *l, *r});
                    }
                }
            };

            for (auto& kv : localIndex) {
                const FileMeta* r = nullptr;
                auto it = remoteIndex.find(kv.first);
                if (it != remoteIndex.end()) r = &it->second;
                consider(kv.first, &kv.second, r);
            }
            for (auto& kv : remoteIndex) {
                if (localIndex.find(kv.first) == localIndex.end())
                    consider(kv.first, nullptr, &kv.second);
            }

            // -----------------------------------------------------
            // 4. Execute plan
            // -----------------------------------------------------
            SyncProgress sp;
            sp.filesTotal = static_cast<std::uint32_t>(plan.size());
            for (auto& op : plan) {
                sp.bytesTotal += (op.kind == Op::Upload) ? op.local.size
                                : (op.kind == Op::Download) ? op.remote.size
                                : 0;
            }

            auto report = [&]() {
                if (onProgress) onProgress(sp);
            };

            for (auto& op : plan) {
                if (cancel->load()) break;
                sp.currentPath = op.rel;

                try {
                    switch (op.kind) {
                    case Op::Upload: {
                        sp.currentOperation = "upload";

                        if (opts.dryRun) {
                            sp.currentOperation = "would-upload";
                            result.uploaded++;
                            sp.bytesDone += op.local.size;
                            sp.filesDone++;
                            report();
                            break;
                        }

                        const auto lp = localDir / fs::path(op.rel.substr(1));
                        CopyOptions cOpts;
                        cOpts.verifyHash = opts.verifyHash;
                        cOpts.preserveTimes = opts.preserveTimes;

                        auto f = self->upload(lp, remoteDir + op.rel, cOpts,
                            [&](const TransferProgress& tp) {
                                sp.currentFile = tp;
                                report();
                            }, cancel);
                        auto r = f.get();
                        result.uploaded++;
                        result.bytesTransferred += r.bytesTransferred;
                        sp.bytesDone += r.bytesTransferred;
                        sp.filesDone++;
                        report();
                        break;
                    }
                    case Op::Download: {
                        sp.currentOperation = "download";

                        if (opts.dryRun) {
                            sp.currentOperation = "would-download";
                            result.downloaded++;
                            sp.bytesDone += op.remote.size;
                            sp.filesDone++;
                            report();
                            break;
                        }

                        const auto lp = localDir / fs::path(op.rel.substr(1));
                        CopyOptions cOpts;
                        cOpts.verifyHash = opts.verifyHash;
                        cOpts.preserveTimes = opts.preserveTimes;

                        auto f = self->download(remoteDir + op.rel, lp, cOpts,
                            [&](const TransferProgress& tp) {
                                sp.currentFile = tp;
                                report();
                            }, cancel);
                        auto r = f.get();
                        result.downloaded++;
                        result.bytesTransferred += r.bytesTransferred;
                        sp.bytesDone += r.bytesTransferred;
                        sp.filesDone++;
                        report();
                        break;
                    }
                    case Op::DeleteRemote: {
                        sp.currentOperation = "delete-remote";

                        if (!opts.deleteExtra) {
                            sp.currentOperation = "skip-delete-remote";
                            result.skipped++;
                            sp.filesDone++;
                            report();
                            break;
                        }

                        if (opts.dryRun) {
                            sp.currentOperation = "would-delete-remote";
                            result.deleted++;
                            sp.filesDone++;
                            report();
                            break;
                        }

                        self->deleteRemote(remoteDir + op.rel, true).get();
                        result.deleted++;
                        sp.filesDone++;
                        report();
                        break;
                    }
                    case Op::DeleteLocal: {
                        sp.currentOperation = "delete-local";

                        if (!opts.deleteExtra) {
                            sp.currentOperation = "skip-delete-local";
                            result.skipped++;
                            sp.filesDone++;
                            report();
                            break;
                        }

                        if (opts.dryRun) {
                            sp.currentOperation = "would-delete-local";
                            result.deleted++;
                            sp.filesDone++;
                            report();
                            break;
                        }

                        std::error_code rec;
                        fs::remove_all(localDir / fs::path(op.rel.substr(1)), rec);
                        result.deleted++;
                        sp.filesDone++;
                        report();
                        break;
                    }
                    case Op::Skip:
                        result.skipped++;
                        sp.filesDone++;
                        report();
                        break;
                    }
                } catch (const std::exception& e) {
                    result.failed++;
                    result.errors.push_back(op.rel + ": " + e.what());
                    sp.filesDone++;
                    report();
                }
            }

            pr->set_value(std::move(result));
        } catch (...) {
            pr->set_exception(std::current_exception());
        }
    }).detach();

    return fut;
}

} // namespace filexferlib
