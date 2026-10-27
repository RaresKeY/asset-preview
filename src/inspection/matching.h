#pragma once
#include <algorithm>
#include <cstddef>
#include <vector>

namespace inspection {
enum class MatchResult { Match, Mismatch, Exhausted };

// Prove that every child has a distinct counterpart. A bounded augmenting-path
// search handles repeated labels/transforms without assuming that pairwise
// similarity is transitive. The caller still owns exact edge verification.
template<class Edge>
MatchResult matchChildren(const std::vector<int>& sources,const std::vector<int>& targets,Edge edge,int& remaining) {
    if(sources.size()!=targets.size()) return MatchResult::Mismatch;
    if(sources.size()>1000000) return MatchResult::Exhausted;
    const int count=int(sources.size());
    std::vector<int> owner(count,-1), chosen(count,-1), predecessor(count), queue;
    queue.reserve(count);
    for(int root=0;root<count;++root) {
        std::fill(predecessor.begin(),predecessor.end(),-1);
        queue.clear(); queue.push_back(root);
        bool found=false;
        for(std::size_t head=0;head<queue.size() && !found;++head) {
            const int target=queue[head];
            for(int source=0;source<count;++source) {
                if(remaining<=0) return MatchResult::Exhausted;
                --remaining;
                if(predecessor[source]>=0) continue;
                const auto match=edge(sources[source],targets[target]);
                if(match==MatchResult::Exhausted) return match;
                if(match==MatchResult::Mismatch) continue;
                predecessor[source]=target;
                if(owner[source]>=0) { queue.push_back(owner[source]); continue; }
                int next=source;
                do {
                    const int destination=predecessor[next], previous=chosen[destination];
                    chosen[destination]=next; owner[next]=destination; next=previous;
                } while(next>=0);
                found=true; break;
            }
        }
        if(!found) return MatchResult::Mismatch;
    }
    return MatchResult::Match;
}
}
