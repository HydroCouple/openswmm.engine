#pragma once
#include <chrono>
#include <map>
#include <string>
#include <cstdio>
namespace review_profile {
struct Entry { double seconds=0; long calls=0; };
inline std::map<std::string, Entry> entries;
struct Timer {
 const char* name;
 std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();
 Timer(const char* n):name(n) {}
 ~Timer() { auto& e=entries[name]; e.seconds+=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count(); ++e.calls; }
};
struct Reporter {
 ~Reporter() { for(auto& [k,v]:entries) std::fprintf(stderr,"PROFILE %s %.9g %ld\n",k.c_str(),v.seconds,v.calls); }
};
inline Reporter reporter;
}
