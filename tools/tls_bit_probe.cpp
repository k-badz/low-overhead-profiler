/*
 * Copyright (c) 2025-2026 Krzysztof Badziak
 *
 * Licensed under the MIT License:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

// custom_tls bit probe (diagnostic tool).
//
// The hot path maps each thread to a slot via lop_tls_index(lop_raw_tid()): a contiguous
// bit-window of the OS thread id. The right window is platform- and layout-dependent (on Linux the
// id is the glibc thread pointer, spaced by the stack stride; on Windows it is the TEB thread id, a
// multiple of 4). This tool spawns as many threads as the OS allows, holds them ALL alive at once,
// and from that snapshot reports:
//   * which id bits actually vary,
//   * the id stride and its trailing-zero count (= the optimal shift; lower bits are constant),
//   * the minimal collision-free contiguous window at shift 0 / the current shift / the optimal
//     shift, and the table size it implies,
//   * a small collisions sweep over (shift, width) candidates.
//
// Use it to choose the index bits so that simultaneously-live threads never share a slot (a shared
// slot means a shared EventBuffer -> data race in the lockless hot path). It only uses the inline
// index helpers, so it does NOT define LOP_IMPLEMENTATION (no engine spun up).
//
//   Build:  g++ tools/tls_bit_probe.cpp -std=c++17 -Iinclude -O2 -pthread -o probe   (Linux)
//           cl /std:c++17 /EHsc /O2 /Iinclude tools\tls_bit_probe.cpp /Fe:probe.exe \
//              /link /STACK:0x10000,0x1000                                            (Windows; small
//                                          per-thread stacks let many more threads start)
//   Run:    ./probe [max_threads_to_try]   (default 200000; it stops at the OS cap)

#include "profiler.h"
#include <thread>
#include <vector>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_set>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <system_error>
#if defined(_MSC_VER)
#  include <intrin.h>
#endif
using namespace LOP;

static int ctz64(uint64_t x){
    if(!x) return 0;
#if defined(_MSC_VER)
    unsigned long idx; _BitScanForward64(&idx,x); return (int)idx;
#else
    return __builtin_ctzll(x);
#endif
}

// Spawn 'want' threads, hold them ALL alive simultaneously, and return each one's lop_raw_tid().
static std::vector<uint64_t> snapshot(int want){
    std::vector<uint64_t> tid(want,0);
    std::vector<std::thread> th; th.reserve(want);
    std::atomic<int> ready{0}; std::mutex m; std::condition_variable cv; bool go=false; int sp=0;
    for(int i=0;i<want;++i){
        try{ th.emplace_back([&,i]{ tid[i]=lop_raw_tid();
              ready.fetch_add(1,std::memory_order_release);
              std::unique_lock<std::mutex> lk(m); cv.wait(lk,[&]{return go;}); }); ++sp; }
        catch(const std::system_error&){ break; } // hit the OS thread cap
    }
    while(ready.load(std::memory_order_acquire)<sp) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    tid.resize(sp);
    { std::lock_guard<std::mutex> lk(m); go=true; } cv.notify_all();
    for(auto&t:th) t.join();
    return tid;
}
static int min_width(const std::vector<uint64_t>&t,int shift){
    for(int W=1;W<=44;++W){ uint64_t mask=(W>=64)?~0ull:((1ull<<W)-1);
        std::unordered_set<uint64_t> s; s.reserve(t.size()*2); bool ok=true;
        for(uint64_t x:t){ if(!s.insert((x>>shift)&mask).second){ok=false;break;} }
        if(ok) return W; }
    return -1;
}
static int collisions(const std::vector<uint64_t>&t,int shift,int W){
    uint64_t mask=(1ull<<W)-1; std::unordered_set<uint64_t> s; s.reserve(t.size()*2); int c=0;
    for(uint64_t x:t) if(!s.insert((x>>shift)&mask).second) ++c; return c;
}
int main(int argc,char**argv){
    int want=argc>1?atoi(argv[1]):200000;
#if defined(_WIN32)||defined(_WIN64)
    const char* os="Windows"; int cur_shift=0;
#else
    const char* os="Linux"; int cur_shift=12;
#endif
    auto tid=snapshot(want);
    int n=(int)tid.size();
    printf("[%s] CUSTOM_TLS_SIZE=%d, current shift=%d, spawned=%d concurrent threads\n",os,CUSTOM_TLS_SIZE,cur_shift,n);
    if(n<2){ printf("too few threads to analyze\n"); return 0; }
    int lo=-1,hi=-1; for(int b=0;b<64;++b){ size_t o=0; for(uint64_t t:tid) o+=(t>>b)&1; if(o!=0&&o!=tid.size()){ if(lo<0)lo=b; hi=b; } }
    printf("varying id bits: [%d..%d]\n",lo,hi);
    std::vector<uint64_t> s=tid; std::sort(s.begin(),s.end());
    uint64_t g=0,mn=~0ull; for(size_t i=1;i<s.size();++i){ uint64_t d=s[i]-s[i-1]; if(d&&d<mn)mn=d; uint64_t a=g,b=d; while(b){uint64_t t=a%b;a=b;b=t;} g=a; }
    int tz=ctz64(g);
    printf("sorted-id min gap=0x%llX (%llu), gcd(gaps)=0x%llX -> stride trailing-zeros (optimal shift)=%d\n",
           (unsigned long long)mn,(unsigned long long)mn,(unsigned long long)g,tz);
    printf("min collision-free width: shift0=%d, shiftCurrent(%d)=%d, shiftOptimal(%d)=%d\n",
           min_width(tid,0),cur_shift,min_width(tid,cur_shift),tz,min_width(tid,tz));
    int wopt=min_width(tid,tz);
    if(wopt>0) printf("=> optimal: shift=%d width=%d -> bits[%d..%d], table 2^%d=%llu (for %d live threads)\n",
                      tz,wopt,tz,tz+wopt-1,wopt,(unsigned long long)(1ull<<wopt),n);
    printf("candidate collisions (shift,width):\n");
    int shifts[3]={0,cur_shift,tz};
    for(int k=0;k<3;++k){ int sh=shifts[k];
        for(int W=14;W<=18;++W) printf("  shift=%2d width=%2d (2^%d=%6llu): collisions=%d\n",
                                       sh,W,W,(unsigned long long)(1ull<<W),collisions(tid,sh,W)); }
    return 0;
}
