#include "inspection/matching.h"
#include <array>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <stdexcept>

using inspection::MatchResult;
void expect(bool condition,const char* reason) { if(!condition) throw std::runtime_error(reason); }
int main() {
    try {
        int budget=0;
        expect(inspection::matchChildren({}, {}, [](int,int) { return MatchResult::Match; },budget)==MatchResult::Match,"empty topology");
        expect(inspection::matchChildren({0},{0},[](int,int) { return MatchResult::Match; },budget)==MatchResult::Exhausted,"budget fails closed");
        budget=100;
        expect(inspection::matchChildren({0},{0},[](int,int) { return MatchResult::Exhausted; },budget)==MatchResult::Exhausted,"nested exhaustion fails closed");
        std::uint32_t random=0x9163abcd;
        for(int count=1;count<=6;++count) for(int sample=0;sample<300;++sample) {
            std::array<std::array<bool,6>,6> graph{};
            for(int i=0;i<count;++i) for(int j=0;j<count;++j) {
                random=random*1664525+1013904223;
                graph[i][j]=sample%3==0 || (sample%3==1 && ((random>>16)&3)==0) || (sample%3==2 && ((random>>27)&1)==0);
            }
            std::vector<int> nodes(count); std::iota(nodes.begin(),nodes.end(),0);
            auto permutation=nodes; bool exact=false;
            do {
                bool valid=true;
                for(int j=0;j<count;++j) valid=valid && graph[permutation[j]][j];
                if(valid) { exact=true; break; }
            } while(std::next_permutation(permutation.begin(),permutation.end()));
            budget=100000;
            const auto result=inspection::matchChildren(nodes,nodes,[&](int i,int j) { return graph[i][j]?MatchResult::Match:MatchResult::Mismatch; },budget);
            expect(result==(exact?MatchResult::Match:MatchResult::Mismatch),"one-to-one proof disagrees with exhaustive permutations");
        }
        budget=1000;
        expect(inspection::matchChildren({0,0,1},{0,1,1},[](int i,int j) { return i==j?MatchResult::Match:MatchResult::Mismatch; },budget)==MatchResult::Mismatch,
            "existence in both directions cannot substitute for a distinct counterpart");
        std::cout<<"Inspection matching agrees with 1800 exhaustive graph references and bounded failure checks\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
