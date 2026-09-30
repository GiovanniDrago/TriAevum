#include "fast/immerse/mxao_shader.h"

// iMMERSE MXAO, ported from MartysMods_MXAO.fx of
// https://github.com/martymcmodding/iMMERSE at revision
// f57d3afa1ebe5d1fd6152d4f6fb9a2e75bd1d1cb.
//
// Copyright (c) Pascal Gilcher. All rights reserved.
// Integrated and modified with the author's explicit permission; see
// THIRD_PARTY_NOTICES.md ("iMMERSE").
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
//
// Changes from the ReShade original: native FP32 depth and the renderer's
// exact projection replace ReShade's linearized depth and assumed 60-degree
// field of view; native view-space normals replace normals reconstructed from
// depth, with the RGB8 decoding bound of the GTAO kernels; fade uses native
// distances; the compute path runs one texel per thread and taps use the
// position of the tile texel they read. Each slice's occlusion is measured
// against its own unoccluded integral (the original's ground-truth mode
// normalized by the slice count and darkened empty oblique planes), and
// occlusion rather than visibility is stored so an unoccluded pixel stays
// exactly zero through half-precision storage and the filter.

namespace Fast::Immerse {
namespace {
constexpr const char* kCommon = R"glsl(#version 450
layout(set=0,binding=0) uniform texture2D native_depth;
layout(set=0,binding=1) uniform texture2D view_normals;
layout(set=0,binding=2) uniform texture2D intermediate;
layout(set=0,binding=4) uniform sampler point_sampler;
layout(push_constant) uniform MxaoState {
    uvec2 extent;
    uint method;
    uint flags;
    vec4 view_x;
    vec4 view_y;
    vec4 depth;
    vec4 radius_fade;
    vec4 response;
} pc;
const int tiles=4;
const float pi=3.14159265358979323846;
// Sky and invalid depth: far enough to never occlude, finite for the math.
const float sky_z=1.0e8;
float view_depth(ivec2 pixel) {
    pixel=clamp(pixel,ivec2(0),ivec2(pc.extent)-1);
    float d=texelFetch(sampler2D(native_depth,point_sampler),pixel,0).r;
    if(d>=1.0 || d<0.0 || isnan(d)) return sky_z;
    return pc.depth.w!=0.0 ? pc.depth.x+d*pc.depth.z :
        pc.depth.x/max(pc.depth.y-d*pc.depth.z,1.0e-12);
}
ivec2 tile_size() {
    return (ivec2(pc.extent)+tiles-1)/tiles;
}
ivec2 deinterleave(ivec2 pixel) {
    return (pixel%tiles)*tile_size()+pixel/tiles;
}
ivec2 reinterleave(ivec2 tiled) {
    ivec2 size=tile_size();
    return (tiled%size)*tiles+tiled/size;
}
vec2 pixel_uv(ivec2 pixel) {
    return (vec2(pixel)+0.5)/vec2(pc.extent);
}
// View space mirrored along Z: MXAO's projection space with positive depth.
vec3 proj_position(ivec2 pixel,float z) {
    vec3 uv=vec3(pixel_uv(pixel),1.0);
    return vec3(dot(pc.view_x.xyz,uv)*z,dot(pc.view_y.xyz,uv)*z,z);
}
)glsl";
} // namespace

std::string BuildMxaoDeinterleaveShader() {
    std::string source = kCommon;
    source += R"glsl(
layout(local_size_x=8,local_size_y=8,local_size_z=1) in;
layout(set=0,binding=3,r32f) uniform writeonly image2D tiled_depth;
void main() {
    ivec2 tiled=ivec2(gl_GlobalInvocationID.xy);
    if(any(greaterThanEqual(tiled,tile_size()*tiles))) return;
    ivec2 pixel=reinterleave(tiled);
    float z=all(lessThan(pixel,ivec2(pc.extent))) ? view_depth(pixel) : sky_z;
    imageStore(tiled_depth,tiled,vec4(z));
}
)glsl";
    return source;
}

std::string BuildMxaoOcclusionShader() {
    std::string source = kCommon;
    source += R"glsl(
layout(local_size_x=16,local_size_y=16,local_size_z=1) in;
layout(set=0,binding=3,rg16f) uniform writeonly image2D raw_occlusion;
//  slices/steps    preset      samples
const uvec2 presets[6]=uvec2[6](
    uvec2(2,2),     // Low        8
    uvec2(2,4),     // Medium     16
    uvec2(2,10),    // High       40
    uvec2(3,12),    // Very High  72
    uvec2(4,14),    // Ultra      112
    uvec2(6,16));   // Extreme    192
float fast_acos(float x) {
    float o=sqrt(max(abs(x)*(abs(x)*0.5405464-3.0079475)+2.4674011,0.0));
    return x<0.0 ? pi-o : o;
}
vec2 fast_acos(vec2 x) {
    return vec2(fast_acos(x.x),fast_acos(x.y));
}
vec3 unit_normal(vec3 encoded) {
    vec3 n=encoded*2.0-1.0;
    return n*inversesqrt(max(dot(n,n),1.0e-12));
}
void main() {
    ivec2 tiled=ivec2(gl_GlobalInvocationID.xy);
    if(any(greaterThanEqual(tiled,tile_size()*tiles))) return;
    ivec2 pixel=reinterleave(tiled);
    if(any(greaterThanEqual(pixel,ivec2(pc.extent)))) return;
    float z=texelFetch(sampler2D(intermediate,point_sampler),tiled,0).r;
    float depth=z>=sky_z ? 1.0 : clamp(z*pc.response.y,0.0,1.0);
    vec4 guide=texelFetch(sampler2D(view_normals,point_sampler),pixel,0);
    float fade=clamp((pc.radius_fade.w-z)/max(pc.radius_fade.w-pc.radius_fade.z,1.0e-5),0.0,1.0);
    if(z>=sky_z || z<=0.0 || guide.a<=0.0 || fade<0.001 || pc.response.x<=0.0) {
        imageStore(raw_occlusion,pixel,vec4(0.0,depth,0.0,0.0));
        return;
    }

    // Depth steps in MXAO's normalization (per mille of the far plane) mark
    // silhouettes and grazing surfaces; the filter smooths those more.
    vec4 neighbours=vec4(view_depth(pixel-ivec2(1,0)),view_depth(pixel+ivec2(1,0)),
                         view_depth(pixel-ivec2(0,1)),view_depth(pixel+ivec2(0,1)));
    vec4 steps=abs(neighbours-z)*(1000.0*pc.response.y);
    vec4 w=steps.xzyw+steps.zywx;
    w=1.0/(0.001+w*w);
    float edge_weight=clamp(1.0-dot(w,vec4(1.0)),0.0,1.0);

    vec3 p=proj_position(pixel,z);
    vec3 n=unit_normal(guide.xyz);
    n.z=-n.z;
    p*=0.996;
    vec3 v=normalize(-p);
    if(dot(n,v)<0.0) n=-n;

    uint preset_index=min(pc.flags&7u,5u);
    uint slice_count=presets[preset_index].x;
    uint sample_count=presets[preset_index].y;

    uint jitter_index=uint(pixel.x%tiles)+uint(tiles)*uint(pixel.y%tiles);
    jitter_index*=11u;
    float jitter=(float(jitter_index%uint(tiles*tiles))+0.5)/float(tiles*tiles);

    vec2 slice_dir=vec2(sin(jitter*pi*(6.0/float(slice_count))),cos(jitter*pi*(6.0/float(slice_count))));
    float rotation_sin=sin(pi/float(slice_count));
    float rotation_cos=cos(pi/float(slice_count));

    // Screen offset of a lateral view-space displacement at this depth.
    mat2 projection=mat2(pc.view_x.x,pc.view_y.x,pc.view_x.y,pc.view_y.y);
    mat2 unprojection=inverse(projection);

    float radius=max(pc.radius_fade.x,1.0e-4);
    float thickness=pc.radius_fade.y;
    float falloff_factor=1.0/(radius*radius);
    float normal_error=pc.response.z;
    vec2 vcrossn_xy=v.yz*n.zx-v.zx*n.yz;
    float ndotv=dot(n,v);
    // Occlusion is accumulated against each slice's unoccluded integral, so
    // an empty hemisphere is exactly zero whatever the slice quadrature.
    float occlusion_sum=0.0;
    float slicesum=0.0;

    for(uint slice=0u;slice<slice_count;++slice) {
        slice_dir=vec2(slice_dir.x*rotation_cos+slice_dir.y*rotation_sin,
                       -slice_dir.x*rotation_sin+slice_dir.y*rotation_cos);
        vec2 radius_pixels=(unprojection*(slice_dir*(radius/p.z)))*vec2(pc.extent);

        float sdotv=dot(slice_dir,v.xy);
        float sdotn=dot(slice_dir,n.xy);
        float ndotns=dot(slice_dir,vcrossn_xy)*inversesqrt(max(1.0-sdotv*sdotv,1.0e-12));
        float sliceweight=sqrt(clamp(1.0-ndotns*ndotns,0.0,1.0));
        float cosn=sliceweight>0.0 ? clamp(ndotv/sliceweight,0.0,1.0) : 1.0;
        float normal_angle=fast_acos(cosn);
        normal_angle=sdotn<sdotv*ndotv ? -normal_angle : normal_angle;

        vec2 unoccludedhorizoncos=vec2(sin(normal_angle),-sin(normal_angle));
        vec2 maxhorizoncos=unoccludedhorizoncos;
        uint occlusion_bitfield=0xFFFFFFFFu;
        vec2 direction=radius_pixels;
        for(int side=0;side<2;++side) {
            maxhorizoncos=maxhorizoncos.yx;
            float lowesthorizoncos=maxhorizoncos.x;
            for(uint s_index=0u;s_index<sample_count;s_index+=2u) {
                vec2 s=(float(s_index)+vec2(0.0,1.0)+jitter)/float(sample_count);
                s*=s;
                // Taps stay on this pixel's tile lattice, like the original's
                // point-sampled deinterleaved texture.
                ivec2 taps[2]=ivec2[2](pixel+ivec2(round(s.x*direction/float(tiles)))*tiles,
                                       pixel+ivec2(round(s.y*direction/float(tiles)))*tiles);
                if(any(lessThan(taps[1],ivec2(0))) || any(greaterThanEqual(taps[1],ivec2(pc.extent)))) break;
                for(int pair=0;pair<2;++pair) {
                    ivec2 tap=taps[pair];
                    float tap_z=texelFetch(sampler2D(intermediate,point_sampler),deinterleave(tap),0).r;
                    vec3 deltavec=proj_position(tap,tap_z)-p;
                    float ddotd=dot(deltavec,deltavec);
                    // Native RGB8 normals have finite angular precision: a tap
                    // inside that uncertainty of the tangent plane is not an
                    // occluder (the same bound as the GTAO kernels).
                    float height=dot(deltavec,n);
                    if(height<=max(z*1.0e-5,1.0e-5) || height*height<=ddotd*normal_error) continue;
                    if(pc.method<2u) {
                        float samplehorizoncos=dot(deltavec,v)*inversesqrt(ddotd);
                        float falloff=1.0/(1.0+ddotd*falloff_factor);
                        samplehorizoncos=mix(lowesthorizoncos,samplehorizoncos,falloff);
                        maxhorizoncos.x=max(maxhorizoncos.x,samplehorizoncos);
                    } else {
                        float ddotv=dot(deltavec,v);
                        vec2 h_frontback=vec2(ddotv,ddotv-thickness)*
                            inversesqrt(max(vec2(ddotd,ddotd-2.0*thickness*ddotv+thickness*thickness),1.0e-20));
                        h_frontback=fast_acos(clamp(h_frontback,-1.0,1.0));
                        h_frontback=side==1 ? h_frontback : -h_frontback.yx;
                        h_frontback=clamp((h_frontback+normal_angle)/pi+0.5,0.0,1.0);
                        // Approximates inverse transform sampling of the cosine lobe.
                        if(pc.method==2u) h_frontback=h_frontback*h_frontback*(3.0-2.0*h_frontback);
                        uint a=min(uint(h_frontback.x*32.0),32u);
                        uint b=min(uint(h_frontback.y*32.0),32u)-a;
                        uint occlusion=a>=32u ? 0u : (b>=32u ? 0xFFFFFFFFu : ((1u<<b)-1u))<<a;
                        occlusion_bitfield&=~occlusion;
                    }
                }
            }
            direction=-direction;
        }
        if(pc.method==0u) {
            // The original normalizes by the slice count; a two-slice
            // quadrature then darkened empty oblique planes by up to 16%.
            vec2 max_horizon_angle=fast_acos(clamp(maxhorizoncos,-1.0,1.0));
            vec2 open_angle=fast_acos(clamp(unoccludedhorizoncos,-1.0,1.0));
            vec2 h=vec2(-max_horizon_angle.x,max_horizon_angle.y);
            vec2 open=vec2(-open_angle.x,open_angle.y);
            vec2 arc=cosn+2.0*h*sin(normal_angle)-cos(2.0*h-normal_angle);
            vec2 open_arc=cosn+2.0*open*sin(normal_angle)-cos(2.0*open-normal_angle);
            occlusion_sum+=max((open_arc.x+open_arc.y)-(arc.x+arc.y),0.0)*sliceweight;
            slicesum+=(open_arc.x+open_arc.y)*sliceweight;
        } else if(pc.method==1u) {
            vec2 max_horizon_angle=fast_acos(clamp(maxhorizoncos,-1.0,1.0));
            vec2 open_angle=fast_acos(clamp(unoccludedhorizoncos,-1.0,1.0));
            occlusion_sum+=max((open_angle.x+open_angle.y)-(max_horizon_angle.x+max_horizon_angle.y),0.0)*
                           sliceweight;
            slicesum+=(open_angle.x+open_angle.y)*sliceweight;
        } else {
            occlusion_sum+=float(32u-min(uint(bitCount(occlusion_bitfield)),32u))/32.0*sliceweight;
            slicesum+=sliceweight;
        }
    }
    float ao=1.0-clamp(occlusion_sum/max(slicesum,1.0e-6),0.0,1.0);

    float amount=pc.response.x;
    ao=mix(1.0,ao,clamp(amount,0.0,1.0));
    // Above 1 the original switches to a gamma response.
    if(amount>1.0) ao=mix(ao,ao*ao,clamp(amount-1.0,0.0,1.0));
    // Stored as occlusion: an unoccluded pixel stays exactly zero through
    // half-precision storage and the (affine) regression filter.
    float occlusion=clamp((1.0-ao)*fade,0.0,1.0);
    imageStore(raw_occlusion,pixel,vec4(occlusion,edge_weight>0.5 ? -depth : depth,0.0,0.0));
}
)glsl";
    return source;
}

std::string BuildMxaoFilterShader(bool finalPass) {
    std::string source = kCommon;
    source += finalPass ? "layout(set=0,binding=3,r32f) uniform writeonly image2D filtered;\n"
                        : "layout(set=0,binding=3,rg16f) uniform writeonly image2D filtered;\n";
    source += R"glsl(
layout(local_size_x=8,local_size_y=8,local_size_z=1) in;
vec2 occlusion(ivec2 pixel) {
    pixel=clamp(pixel,ivec2(0),ivec2(pc.extent)-1);
    return texelFetch(sampler2D(intermediate,point_sampler),pixel,0).rg;
}
void main() {
    ivec2 pixel=ivec2(gl_GlobalInvocationID.xy);
    if(any(greaterThanEqual(pixel,ivec2(pc.extent)))) return;
    vec2 center=occlusion(pixel);
    vec2 result=center;
    if((pc.flags&32u)==0u) {
        // Linear regression of occlusion on depth over a 4x4 block; the
        // mirrored second iteration makes the two-pass footprint symmetric.
        ivec2 origin=pixel-((pc.flags&16u)!=0u ? ivec2(2) : ivec2(1));
        vec4 moments=vec4(0.0);
        for(int y=0;y<4;++y) {
            for(int x=0;x<4;++x) {
                vec2 texel=occlusion(origin+ivec2(x,y));
                float d=abs(texel.y);
                moments+=vec4(d,d*d,texel.x,texel.x*d);
            }
        }
        moments/=16.0;
        bool blurry=center.y<0.0;
        float b=(moments.w-moments.x*moments.z)/max(moments.y-moments.x*moments.x,exp2(blurry ? -12.0 : -30.0));
        float a=moments.z-b*moments.x;
        result=vec2(clamp(b*abs(center.y)+a,0.0,1.0),center.y);
    }
)glsl";
    source += finalPass ? "    imageStore(filtered,pixel,vec4(1.0-result.x));\n}\n"
                        : "    imageStore(filtered,pixel,vec4(result,0.0,0.0));\n}\n";
    return source;
}

} // namespace Fast::Immerse
