#include "fast/immerse/launchpad_albedo_shader.h"

// iMMERSE LAUNCHPAD albedo estimate ("Fake albedo texture"), ported from
// MartysMods_LAUNCHPAD.fx of https://github.com/martymcmodding/iMMERSE at
// revision f57d3afa1ebe5d1fd6152d4f6fb9a2e75bd1d1cb.
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
// Changes from the ReShade original: the scene colour is the declared scene
// input of the AO stage (re-encoded to sRGB when the renderer stores it
// linear), the pyramid levels and the fusion's lowest level follow the
// shorter screen side, and the upsampled estimate is stored as its channel
// mean, the only quantity the textured normals read.

namespace Fast::Immerse {
namespace {
constexpr const char* kCommon = R"glsl(#version 450
layout(local_size_x=8,local_size_y=8,local_size_z=1) in;
layout(set=0,binding=0) uniform texture2D scene_color;
layout(set=0,binding=2) uniform texture2D source;
layout(set=0,binding=4) uniform sampler linear_sampler;
layout(set=0,binding=5) uniform texture2D levels[10];
layout(push_constant) uniform AlbedoState {
    uvec2 extent;
    uvec2 source_extent;
    uint horizontal;
    uint lowest_level;
    uint linear_source;
    uint reserved;
    uvec2 level_extents[10];
} pc;
vec3 cone_overlap(vec3 c) {
    const float k=0.5*0.33;
    const vec2 f=vec2(1.0-2.0*k,k);
    return mat3(f.xyy,f.yxy,f.yyx)*c;
}
// The original's SDR to HDR mapping of display-encoded colour.
vec3 sdr_to_hdr(vec3 c) {
    c=clamp(c,0.0,1.0);
    c=cone_overlap(c);
    c=c*0.283799*((2.52405+c)*c);
    return c/(1.04-clamp(c,0.0,1.0));
}
float average(vec3 c) {
    return dot(c,vec3(0.3333));
}
vec3 encode_srgb(vec3 c) {
    c=clamp(c,0.0,1.0);
    return mix(c*12.92,1.055*pow(c,vec3(1.0/2.4))-0.055,step(vec3(0.0031308),c));
}
// Display-encoded scene colour, as the original reads its back buffer.
vec3 scene(vec2 uv) {
    vec3 c=textureLod(sampler2D(scene_color,linear_sampler),uv,0.0).rgb;
    return pc.linear_source!=0u ? encode_srgb(c) : c;
}
bool destination_texel(out ivec2 texel,out vec2 uv) {
    texel=ivec2(gl_GlobalInvocationID.xy);
    uv=(vec2(texel)+0.5)/vec2(pc.extent);
    return all(lessThan(texel,ivec2(pc.extent)));
}
)glsl";
} // namespace

std::string BuildLaunchpadAlbedoInitShader() {
    std::string source = kCommon;
    source += R"glsl(
layout(set=0,binding=3,rg16f) uniform writeonly image2D destination;
void main() {
    ivec2 texel;
    vec2 uv;
    if(!destination_texel(texel,uv)) return;
    float loglum=dot(vec3(0.3333),log2(max(vec3(1.0e-3),sdr_to_hdr(scene(uv)))));
    imageStore(destination,texel,vec4(-loglum,loglum,0.0,0.0));
}
)glsl";
    return source;
}

std::string BuildLaunchpadAlbedoDownsampleShader() {
    std::string source = kCommon;
    source += R"glsl(
layout(set=0,binding=3,rg16f) uniform writeonly image2D destination;
void main() {
    ivec2 texel;
    vec2 uv;
    if(!destination_texel(texel,uv)) return;
    vec2 texelsize=1.0/vec2(pc.source_extent);
    vec2 axis=pc.horizontal!=0u ? vec2(texelsize.x,0.0) : vec2(0.0,texelsize.y);
    vec4 mL=vec4(0.0);
    vec4 mR=vec4(0.0);
    vec2 wsum=vec2(0.0);
    for(int j=-11;j<=11;j++) {
        vec2 tuv=uv+float(j)*axis;
        float inside=all(greaterThan(tuv-tuv*tuv,vec2(0.0))) ? 1.0 : 0.0;
        float w=exp(-float(j*j)/121.0*3.0)*inside;
        vec2 t=textureLod(sampler2D(source,linear_sampler),tuv,0.0).xy;
        w*=j==0 ? 0.5 : 1.0;
        vec4 m=vec4(t,t*t)*w;
        if(j<=0) {
            mL+=m;
            wsum.x+=w;
        }
        if(j>=0) {
            mR+=m;
            wsum.y+=w;
        }
    }
    mL/=wsum.x;
    mR/=wsum.y;
    // The log-luminance variance of each side weights it: edges stay put.
    float vL=max(0.0,mL.w-mL.y*mL.y);
    float vR=max(0.0,mR.w-mR.y*mR.y);
    vec2 w=1.0/(0.25+sqrt(vec2(vL,vR)));
    imageStore(destination,texel,vec4((mL.xy*w.x+mR.xy*w.y)/(w.x+w.y),0.0,0.0));
}
)glsl";
    return source;
}

std::string BuildLaunchpadAlbedoFuseShader() {
    std::string source = kCommon;
    source += R"glsl(
layout(set=0,binding=3,rg16f) uniform writeonly image2D destination;
vec2 level_tap(int j,vec2 uv) {
    return textureLod(sampler2D(levels[j],linear_sampler),uv,0.0).xy;
}
// B-spline bicubic filtering of the coarse levels.
vec2 level_bspline(int j,vec2 iuv) {
    vec2 size=vec2(pc.level_extents[j]);
    vec4 uv;
    uv.xy=iuv*size;
    vec2 center=floor(uv.xy-0.5)+0.5;
    vec4 d=vec4(uv.xy-center,1.0+center-uv.xy);
    vec4 d2=d*d;
    vec4 d3=d2*d;
    vec4 o=d2*0.12812+d3*0.07188;
    uv.xy=center-o.zw;
    uv.zw=center+1.0+o.xy;
    uv/=size.xyxy;
    vec4 w=0.16666666+d*0.5+0.5*d2-d3*0.3333333;
    w=w.wwyy*w.zxzx;
    return w.x*level_tap(j,uv.xy)+w.y*level_tap(j,uv.zy)+w.z*level_tap(j,uv.xw)+w.w*level_tap(j,uv.zw);
}
vec2 level(int j,vec2 uv) {
    return j<4 ? level_tap(j,uv) : level_bspline(j,uv);
}
float fuse_weight(float a,float b) {
    float res=abs(a-b)/max(max(a,b),1.0);
    res*=0.3;
    return clamp(res/(1.0+res),0.0,1.0);
}
void main() {
    ivec2 texel;
    vec2 uv;
    if(!destination_texel(texel,uv)) return;
    int lowest=int(pc.lowest_level);
    vec2 bias=level(lowest,uv);
    for(int j=lowest-1;j>=0;--j) {
        vec2 g=level(j,uv);
        bias=mix(bias,g,fuse_weight(g.y,bias.y));
    }
    imageStore(destination,texel,vec4(bias.x,average(scene(uv)),0.0,0.0));
}
)glsl";
    return source;
}

std::string BuildLaunchpadAlbedoMainShader() {
    std::string source = kCommon;
    source += R"glsl(
layout(set=0,binding=3,r16f) uniform writeonly image2D destination;
void main() {
    ivec2 texel;
    vec2 uv;
    if(!destination_texel(texel,uv)) return;
    // Guided filter: a local linear model of the fused bias over the guide.
    vec2 texelsize=1.0/vec2(pc.source_extent);
    vec4 m=vec4(0.0);
    float ws=0.0;
    for(int y=-1;y<=1;y++)
        for(int x=-1;x<=1;x++) {
            vec2 t=textureLod(sampler2D(source,linear_sampler),uv+vec2(x,y)*texelsize,0.0).xy;
            float w=exp(-float(x*x+y*y));
            m+=vec4(t.y,t.y*t.y,t.y*t.x,t.x)*w;
            ws+=w;
        }
    m/=ws;
    float a=(m.z-m.x*m.w)/(max(m.y-m.x*m.x,0.0)+0.00001);
    float b=m.w-a*m.x;
    vec3 color=scene(uv);
    float bias=a*average(color)+b;
    bias+=dot(vec3(0.3333),log2(max(vec3(1.0e-3),sdr_to_hdr(vec3(0.18)))));
    vec3 c=sdr_to_hdr(color)*exp2(bias);
    // Constant light with back-scatter probability 0.5: inverting the
    // multiscattering series recovers the albedo's chromaticity.
    vec3 reverse_multiscattered=c/(1.0+c*0.5);
    vec3 albedo=normalize(reverse_multiscattered+1.0e-3)*length(c);
    imageStore(destination,texel,vec4(average(albedo)));
}
)glsl";
    return source;
}

} // namespace Fast::Immerse
