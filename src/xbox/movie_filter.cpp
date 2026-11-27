#include "movie_filter.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace xbox_native {
namespace {
constexpr int block_size=16;
// Require four settled movie pictures before retaining a block. Release
// after at most 0.4 seconds at 30 fps even if the new position persists.
constexpr int settle_frames=4,max_hold_frames=12;
struct Patch {
    const unsigned char* current;
    const unsigned char* previous;
    int pitch,width,height;

    // Skip the outer pixel so that every shifted candidate stays inside
    // this block, including at the edge of an odd-sized overlay.
    // XMV motion compensation can use half-pixel as well as whole-pixel
    // translations. Test those positions without resampling the output.
    int error(int dx,int dy,int stride,int& peak) const {
        int total=0;
        peak=0;
        const int half_x=(dx%2)*2,half_y=(dy%2)*pitch;
        for(int y=1;y<height-1;y+=stride) {
            const auto* a=current+y*pitch;
            const auto* b=previous+(y+dy/2)*pitch;
            for(int x=1;x<width-1;x+=stride) {
                const auto* q=b+2*(x+dx/2);
                const int predicted=(q[0]+q[half_x]+q[half_y]+q[half_y+half_x]+2)/4;
                const int change=std::abs(a[2*x]-predicted);
                total+=change;
                peak=std::max(peak,change);
            }
        }
        return total;
    }
    int shift() const {
        int peak;
        const int count=(width-2)*(height-2);
        const int coarse=error(0,0,3,peak);
        int best=coarse,dx=0,dy=0;
        for(int j=-2;j<=2;++j) for(int i=-2;i<=2;++i) {
            if(!i && !j) continue;
            const int value=error(i,j,3,peak);
            if(value<best) { best=value;dx=i;dy=j; }
        }
        if(best*2>=coarse) return 0;
        const int aligned=error(0,0,1,peak);
        const int translated=error(dx,dy,1,peak);
        if(peak>12 || 4*translated>9*count || 2*translated+count/2>=aligned) return 0;
        // Reject a color change or a newly exposed edge anywhere in the
        // tile, including portions outside the motion-matching samples.
        for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
            const int k=y*pitch+2*x;
            if(std::abs(current[k+1]-previous[k+1])>8) return 0;
            if((x==0 || y==0 || x==width-1 || y==height-1) && std::abs(current[k]-previous[k])>28) return 0;
        }
        return 3*((dy>0)-(dy<0))+(dx>0)-(dx<0);
    }
    bool small_change() const {
        int total=0;
        for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
            const int k=y*pitch+2*x;
            const int delta=std::abs(current[k]-previous[k]);
            if(delta>24 || std::abs(current[k+1]-previous[k+1])>6) return false;
            total+=delta;
        }
        return total<=5*width*height;
    }
};
}

Bytes MovieFilter::frame(const VideoOverlay& video) {
    const auto& g=video.geometry;
    const int width=int(g.width),height=int(g.height),pitch=int(video.pitch);
    const int cols=(width+block_size-1)/block_size,rows=(height+block_size-1)/block_size;
    const auto length=std::size_t(pitch)*(height-1)+((width+1)/2)*4;
    if(width_!=g.width || height_!=g.height || pitch_!=video.pitch || movie_!=video.movie) {
        width_=g.width;height_=g.height;pitch_=video.pitch;movie_=video.movie;
        reference_.assign(video.pairs.begin(),video.pairs.begin()+length);
        blocks_.assign(cols*rows,{});
        return reference_;
    }
    auto patch=[&](int x,int y) {
        const int offset=y*block_size*pitch+2*x*block_size;
        return Patch{reinterpret_cast<const unsigned char*>(video.pairs.data())+offset,
            reinterpret_cast<const unsigned char*>(reference_.data())+offset,pitch,
            std::min(block_size,width-x*block_size),std::min(block_size,height-y*block_size)};
    };
    int stationary_detail=0;
    for(int y=0;y<rows;++y) for(int x=0;x<cols;++x) {
        auto& b=blocks_[y*cols+x];
        const auto p=patch(x,y);
        int low=255,high=0,sum=0,peak=0,count=0;
        for(int j=1;j<p.height-1;j+=3) for(int i=1;i<p.width-1;i+=3) {
            const int k=j*pitch+2*i,v=p.current[k],diff=std::abs(v-p.previous[k]);
            low=std::min(low,v);high=std::max(high,v);sum+=diff;peak=std::max(peak,diff);++count;
        }
        b.quiet=count && peak<=4 && 2*sum<=count;
        b.shift=0;
        b.hold=false;
        if(b.quiet && high-low>=10) ++stationary_detail;
    }
    // A pan or cut changes the surrounding scenery too. Require still
    // detail in the frame and still neighbors outside the candidate block.
    if(stationary_detail>=std::max(4,cols*rows/40)) {
        for(int y=0;y<rows;++y) for(int x=0;x<cols;++x) {
            auto& b=blocks_[y*cols+x];
            if(b.quiet || b.still<settle_frames || b.held>=max_hold_frames) continue;
            const auto p=patch(x,y);
            if(p.width<8 || p.height<8) continue;
            for(const int radius:{3,5}) {
                int quiet=0,neighbors=0;
                for(int j=-radius;j<=radius;j+=radius) for(int i=-radius;i<=radius;i+=radius) {
                    if((!i && !j) || x+i<0 || y+j<0 || x+i>=cols || y+j>=rows) continue;
                    ++neighbors;
                    if(blocks_[(y+j)*cols+x+i].quiet) ++quiet;
                }
                if(quiet>=2 && 3*quiet>=neighbors) {
                    b.shift=static_cast<signed char>(p.shift());
                    break;
                }
            }
        }
        // A tile containing several surfaces may match poorly on its own.
        // Bridge it only when several adjoining tiles agree on a direction.
        for(int y=0;y<rows;++y) for(int x=0;x<cols;++x) {
            const int k=y*cols+x;
            auto& b=blocks_[k];
            if(b.shift) { b.hold=true;continue; }
            if(b.still<settle_frames || b.held>=max_hold_frames) continue;
            std::array<int,9> votes{};
            for(int j=std::max(0,y-1);j<=std::min(rows-1,y+1);++j)
                for(int i=std::max(0,x-1);i<=std::min(cols-1,x+1);++i) {
                    const int direction=blocks_[j*cols+i].shift;
                    if(direction) ++votes[4+direction];
                }
            b.hold=*std::max_element(votes.begin(),votes.end())>=3 && patch(x,y).small_change();
        }
    }
    for(int y=0;y<rows;++y) for(int x=0;x<cols;++x) {
        auto& b=blocks_[y*cols+x];
        if(b.hold) { ++b.held;continue; }
        b.held=0;
        b.still=b.quiet ? std::min(settle_frames,int(b.still)+1) : 0;
        const auto p=patch(x,y);
        const auto bytes=((p.width+1)/2)*4;
        auto* dest=reference_.data()+y*block_size*pitch+2*x*block_size;
        for(int j=0;j<p.height;++j) std::memcpy(dest+j*pitch,p.current+j*pitch,bytes);
    }
    return reference_;
}
}
