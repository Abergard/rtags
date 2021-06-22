/* This file is part of RTags (https://github.com/Andersbakken/rtags).

   RTags is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   RTags is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with RTags.  If not, see <https://www.gnu.org/licenses/>. */

#include "IndexerJob.h"

#include <limits.h>
#include <string.h>
#include <map>
#include <unordered_map>
#include <utility>

#include "CompilerManager.h"
#include "Project.h"
#include "RTags.h"
#include "Server.h"
#include "RTagsVersion.h"
#include "Location.h"
#include <vector>
#include "rct/Serializer.h"

uint64_t IndexerJob::sNextId = 1;
IndexerJob::IndexerJob(const SourceList &s,
                       Flags<Flag> f,
                       const std::shared_ptr<Project> &p,
                       const UnsavedFiles &u)
    : id(0), flags(f),
      project(p->path()), unsavedFiles(u), crashCount(0), mCachedPriority(INT_MIN)
{
    sources.push_back(s.front());
    for (size_t i=1; i<s.size(); ++i) {
        const Source &src = s.at(i);
        bool found = false;
        for (size_t j=0; j<sources.size(); ++j) {
            if (src.compareArguments(sources.at(j))) {
                found = true;
                break;
            }
        }
        if (!found)
            sources.push_back(src);
    }

    assert(!sources.empty());
    sourceFile = s.begin()->sourceFile();
    acquireId();
    visited.insert(sources.begin()->fileId);
}

IndexerJob::~IndexerJob()
{
    destroyed(this);
}

void IndexerJob::acquireId()
{
    id = sNextId++;
}

int IndexerJob::priority() const
{
    if (mCachedPriority == INT_MIN) {
        int ret = 0;
        Server *server = Server::instance();
        uint32_t fileId = sources.begin()->fileId;
        assert(server);
        if (flags & Dirty) {
            ++ret;
        } else if (flags & Reindex) {
            ret += 4;
        }
        std::shared_ptr<Project> p = server->project(project);
        switch (server->activeBufferType(fileId)) {
        case Server::Active:
            ret += 8;
            break;
        case Server::Open:
            ret += 3;
            break;
        case Server::Inactive:
            if (DependencyNode *node = p->dependencyNode(fileId)) {
                std::set<DependencyNode*> seen;
                seen.insert(node);
                std::function<bool(const DependencyNode *node)> func = [&seen, server, &func](const DependencyNode *n) {
                    for (const auto &inc : n->includes) {
                        if (seen.insert(inc.second).second
                            && !Location::path(n->fileId).isSystem()
                            && (server->activeBufferType(n->fileId) != Server::Inactive || func(inc.second))) {
                            return true;
                        }
                    }
                    return false;
                };
                if (func(node))
                    ret += 2;
            }
        }

        if (p && server->currentProject() == p)
            ++ret;
        mCachedPriority = ret;
    }
    return mCachedPriority;
}

String IndexerJob::encode() const
{
    String ret;
    {
        Serializer serializer(ret);
        serializer.write("1234", sizeof(int)); // for size
        std::shared_ptr<Project> proj = Server::instance()->project(project);
        const Server::Options &options = Server::instance()->options();
        serializer << static_cast<uint16_t>(RTags::DatabaseVersion)
                   << options.sandboxRoot
                   << id
                   << options.socketFile
                   << project
                   << static_cast<uint32_t>(sources.size());
        for (Source copy : sources) {
            if (!(options.options & Server::AllowWErrorAndWFatalErrors)) {
                auto it = std::find(copy.arguments.begin(), copy.arguments.end(), "-Werror");
                if (it != copy.arguments.end())
                    copy.arguments.erase(it);
                it = std::find(copy.arguments.begin(), copy.arguments.end(), "-Wfatal-errors");
                if (it != copy.arguments.end())
                    copy.arguments.erase(it);
            }
            copy.arguments.insert(copy.arguments.end(), options.defaultArguments.begin(), options.defaultArguments.end());

            if (!(options.options & Server::AllowPedantic)) {
                const auto it = std::find(copy.arguments.begin(), copy.arguments.end(), "-Wpedantic");
                if (it != copy.arguments.end()) {
                    copy.arguments.erase(it);
                }
            }

            if (options.options & Server::EnableCompilerManager) {
                CompilerManager::applyToSource(copy, CompilerManager::IncludeIncludePaths);
            }

            Server::instance()->filterBlockedArguments(copy);
            copy.includePaths.insert(copy.includePaths.begin(), options.includePaths.begin(), options.includePaths.end());
            if (Server::instance()->options().options & Server::PCHEnabled)
                proj->fixPCH(copy);

            for (const auto &ref : options.defines) {
                copy.defines.insert(ref);
            }
            if (!(options.options & Server::EnableNDEBUG)) {
                copy.defines.erase(Source::Define("NDEBUG"));
            }
            assert(!sourceFile.isEmpty());
            copy.encode(serializer, Source::IgnoreSandbox);
        }
        assert(proj);
        serializer << sourceFile
                   << flags
                   << static_cast<uint32_t>(options.rpVisitFileTimeout)
                   << static_cast<uint32_t>(options.rpIndexDataMessageTimeout)
                   << static_cast<uint32_t>(options.rpConnectTimeout)
                   << static_cast<uint32_t>(options.rpConnectAttempts)
                   << static_cast<int32_t>(options.rpNiceValue)
                   << options.options
                   << unsavedFiles
                   << options.dataDir
                   << options.debugLocations;

        proj->encodeVisitedFiles(serializer);
    }
    const uint32_t size = ret.size() - sizeof(int);
    memcpy(&ret[0], &size, sizeof(size));
    return ret;
}

String IndexerJob::dumpFlags(Flags<Flag> flags)
{
    std::vector<String> ret;
    if (flags & Dirty) {
        ret.push_back("Dirty");
    }
    if (flags & Reindex) {
        ret.push_back("Reindex");
    }
    if (flags & Compile) {
        ret.push_back("Compile");
    }
    if (flags & Running) {
        ret.push_back("Running");
    }
    if (flags & Crashed) {
        ret.push_back("Crashed");
    }
    if (flags & Aborted) {
        ret.push_back("Aborted");
    }
    if (flags & Complete) {
        ret.push_back("Complete");
    }

    return String::join(ret, ", ");
}

void IndexerJob::recalculatePriority()
{
    mCachedPriority = INT_MIN;
    priority();
}
