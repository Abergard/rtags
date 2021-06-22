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

#include "ListSymbolsJob.h"

#include <stddef.h>
#include <stdint.h>
#include <algorithm>
#include <functional>
#include <set>
#include <vector>

#include "Project.h"
#include "QueryMessage.h"
#include <vector>
#include "RTags.h"
#include "FileMap.h"
#include "Location.h"
#include "Symbol.h"
#include "rct/Flags.h"
#include "rct/Path.h"

const Flags<QueryJob::JobFlag> defaultFlags = (QueryJob::WriteUnfiltered | QueryJob::QuietJob);
const Flags<QueryJob::JobFlag> elispFlags = (defaultFlags | QueryJob::QuoteOutput);

ListSymbolsJob::ListSymbolsJob(const std::shared_ptr<QueryMessage> &query, const std::shared_ptr<Project> &proj)
    : QueryJob(query, proj, query->flags() & QueryMessage::Elisp ? elispFlags : defaultFlags),
      string(query->query())
{
}

int ListSymbolsJob::execute()
{
    std::set<String> out;
    std::shared_ptr<Project> proj = project();
    if (proj) {
        if (queryFlags() & QueryMessage::WildcardSymbolNames
            && (string.contains('*') || string.contains('?')) && !string.endsWith('*')) {
            string += '*';
        }
        std::vector<QueryMessage::PathFilter> filters = pathFilters();
        std::vector<Path> paths;
        for (const auto &filter : filters) {
            if (filter.mode == QueryMessage::PathFilter::Self) {
                paths.push_back(filter.pattern);
                if (!paths.back().isFile()) {
                    paths.clear();
                    break;
                }
            } else {
                paths.clear();
                break;
            }
        }
        if (!paths.empty()) {
            out = listSymbolsWithPathFilter(proj, paths);
        } else {
            out = listSymbols(proj);
        }
    }

    if (queryFlags() & QueryMessage::Elisp) {
        write("(list", IgnoreMax | DontQuote);
        for (std::set<String>::const_iterator it = out.begin(); it != out.end(); ++it) {
            write(*it);
        }
        write(")", IgnoreMax | DontQuote);
    } else {
        std::vector<String> sorted;
        sorted.reserve(out.size());
        for (const auto &ref : out) {
            sorted.push_back(ref);
        }
        if (queryFlags() & QueryMessage::ReverseSort) {
            std::sort(sorted.begin(), sorted.end(), std::greater<String>());
        } else {
            std::sort(sorted.begin(), sorted.end());
        }
        const int count = sorted.size();
        for (int i = 0; i < count; ++i) {
            write(sorted.at(i));
        }
    }
    return out.empty() ? 1 : 0;
}

std::set<String> ListSymbolsJob::listSymbolsWithPathFilter(const std::shared_ptr<Project> &project, const std::vector<Path> &paths) const
{
    std::set<String> out;
    const bool wildcard = queryFlags() & QueryMessage::WildcardSymbolNames && (string.contains('*') || string.contains('?'));
    const bool stripParentheses = queryFlags() & QueryMessage::StripParentheses;
    const bool caseInsensitive = queryFlags() & QueryMessage::MatchCaseInsensitive;
    const String::CaseSensitivity cs = caseInsensitive ? String::CaseInsensitive : String::CaseSensitive;
    for (size_t i=0; i<paths.size(); ++i) {
        const Path file = paths.at(i);
        const uint32_t fileId = Location::fileId(file);
        if (!fileId)
            continue;
        auto symbols = project->openSymbols(fileId);
        if (!symbols)
            continue;
        const int count = symbols->count();
        for (int j=0; j<count; ++j) {
            const Symbol &symbol = symbols->valueAt(j);
            if (!filterKind(symbol)) {
                continue;
            }
            const String &symbolName = symbol.symbolName;
            if (symbolName.isEmpty())
                continue;
            if (!string.isEmpty()) {
                if (wildcard) {
                    if (!Project::matchSymbolName(string, symbolName, cs)) {
                        continue;
                    }
                } else if (!symbolName.contains(string, cs)) {
                    continue;
                }
            }

            if (stripParentheses) {
                const int paren = symbolName.indexOf('(');
                if (paren == -1) {
                    out.insert(symbolName);
                } else {
                    if (!RTags::isFunctionVariable(symbolName))
                        out.insert(symbolName.left(paren));
                }
            } else {
                out.insert(symbolName);
            }
        }
    }
    return out;
}

std::set<String> ListSymbolsJob::listSymbols(const std::shared_ptr<Project> &project) const
{
    const bool hasFilter = QueryJob::hasFilter();
    const bool hasKindFilter = QueryJob::hasKindFilter();
    const bool stripParentheses = queryFlags() & QueryMessage::StripParentheses;

    std::set<String> out;
    auto inserter = [this, &project, hasFilter, hasKindFilter, stripParentheses, &out](Project::SymbolMatchType,
                                                                                       const String &str,
                                                                                       const std::set<Location> &locations) {
        if (hasFilter) {
            bool ok = false;
            for (const auto &l : locations) {
                if (filter(l.path())) {
                    ok = true;
                    break;
                }
            }
            if (!ok)
                return;
        }
        if (hasKindFilter) {
            const Symbol sym = project->findSymbol(*locations.begin());
            if (!filterKind(sym))
                return;
        }
        const int paren = str.indexOf('(');
        if (paren == -1) {
            out.insert(str);
        } else {
            if (!RTags::isFunctionVariable(str))
                out.insert(str.left(paren));
            if (!stripParentheses)
                out.insert(str);
        }
    };

    project->findSymbols(string, inserter, queryFlags());
    return out;
}
