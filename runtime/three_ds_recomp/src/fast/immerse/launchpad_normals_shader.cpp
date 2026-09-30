#include "fast/immerse/launchpad_normals_shader.h"

// iMMERSE LAUNCHPAD normal smoothing ("smooth_normals_mkii"), ported from
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
// Changes from the ReShade original: the native view-space normal guide
// replaces normals reconstructed from depth, and the smoothing leaves pixels
// without a native surface (sky, uncovered) as they are; positions come from
// native depth and the renderer's exact projection, measured in near-plane
// units as ReShade's linearized depth is; the kernel samples fixed screen
// directions instead of projected tangent directions and is isotropic in
// pixels, its radius a fraction of the shorter screen side; the half-height
// G-buffer and the wide result pack a 16-bit octahedral normal with 32-bit
// depth instead of RGBA16F, and taps use the position of the texel they read.
// Texture detail ("textured normals") can also refine the native normal
// without the smoothing; its spiral is laid out on screen (the original's
// tangent-plane step over the pixel footprint) rather than projected from the
// tangent plane; it reads the albedo estimate's channel mean and the native
// depth of the nearest pixel instead of depth packed in the albedo, and skips
// samples off screen or on the sky, which the original only weighs down.

namespace Fast::Immerse {
namespace {
constexpr const char* kCommon = R"glsl(#version 450
#extension GL_EXT_control_flow_attributes : require
layout(local_size_x=8,local_size_y=8,local_size_z=1) in;
layout(set=0,binding=0) uniform texture2D native_depth;
layout(set=0,binding=1) uniform texture2D view_normals;
// Half-height packed surfaces: the G-buffer, or the wide iteration's result.
layout(set=0,binding=2) uniform utexture2D surfaces;
layout(set=0,binding=4) uniform sampler point_sampler;
// Texture detail: the albedo estimate's channel mean, sampled bilinearly.
layout(set=0,binding=5) uniform texture2D albedo;
layout(set=0,binding=6) uniform sampler linear_sampler;
layout(push_constant) uniform LaunchpadState {
    uvec2 extent;
    uint iteration;
    uint flags;
    vec4 view_x;
    vec4 view_y;
    vec4 depth;
    vec4 scale;
    vec4 detail;
} pc;
const float tau=6.28318530717958647692;
const float sky_z=1.0e8;
float view_depth(ivec2 pixel) {
    float d=texelFetch(sampler2D(native_depth,point_sampler),pixel,0).r;
    if(d>=1.0 || d<0.0 || isnan(d)) return sky_z;
    return pc.depth.w!=0.0 ? pc.depth.x+d*pc.depth.z :
        pc.depth.x/max(pc.depth.y-d*pc.depth.z,1.0e-12);
}
// LAUNCHPAD's projection space: view space mirrored along Z, measured in
// near-plane units (ReShade's linearized depth is one at the near plane).
vec3 proj_position(ivec2 pixel,float z) {
    vec3 uv=vec3((vec2(pixel)+0.5)/vec2(pc.extent),1.0);
    return vec3(dot(pc.view_x.xyz,uv),dot(pc.view_y.xyz,uv),1.0)*z;
}
// A native surface: a covered guide texel with finite depth.
bool native_surface(ivec2 pixel,out vec3 p,out vec3 n) {
    vec4 guide=texelFetch(sampler2D(view_normals,point_sampler),pixel,0);
    float z=view_depth(pixel);
    n=guide.xyz*2.0-1.0;
    float length2=dot(n,n);
    p=vec3(0.0);
    if(guide.a<=0.0 || z>=sky_z || z<=0.0 || length2<1.0e-6) return false;
    n*=inversesqrt(length2);
    n.z=-n.z;
    p=proj_position(pixel,z/pc.scale.x);
    return true;
}
vec2 octahedral_enc(vec3 v) {
    vec2 result=v.xy/dot(abs(v),vec3(1.0));
    vec2 sgn=vec2(v.x>=0.0 ? 1.0 : -1.0,v.y>=0.0 ? 1.0 : -1.0);
    result=v.z<0.0 ? sgn-abs(result.yx)*sgn : result;
    return result*0.5+0.5;
}
vec3 octahedral_dec(vec2 o) {
    o=o*2.0-1.0;
    vec3 v=vec3(o,1.0-abs(o.x)-abs(o.y));
    float t=clamp(-v.z,0.0,1.0);
    v.xy+=vec2(v.x>=0.0 ? -t : t,v.y>=0.0 ? -t : t);
    return normalize(v);
}
// Packed surface: octahedral normal and depth in near-plane units; zero
// depth marks a texel without a surface.
uvec2 pack_surface(vec3 n,float z) {
    return uvec2(packUnorm2x16(octahedral_enc(n)),floatBitsToUint(z));
}
// Texel of every other row; the position is that row's pixel.
bool packed_surface(ivec2 texel,out vec3 p,out vec3 n) {
    uvec2 s=texelFetch(usampler2D(surfaces,point_sampler),texel,0).xy;
    float z=uintBitsToFloat(s.y);
    n=octahedral_dec(unpackUnorm2x16(s.x));
    p=vec3(0.0);
    if(!(z>0.0)) return false;
    p=proj_position(ivec2(texel.x,texel.y*2),z);
    return true;
}
void to_tangent(vec3 n,out vec3 u,out vec3 v) {
    bool bestside=n.z<n.y;
    vec3 n2=bestside ? n.xzy : n;
    vec3 k=(-n2.xxy*n2.xyy)/(1.0+n2.z)+vec3(1.0,0.0,1.0);
    u=vec3(k.xy,-n2.x);
    v=vec3(k.yz,-n2.y);
    if(bestside) {
        u=u.xzy;
        v=v.xzy;
    }
}
// Projects neighbouring points onto the plane tangent to the surface and fits
// a local linear model of how their normals lean away from the centre normal,
// then evaluates the model at the centre rather than at the centroid of the
// points, which keeps grazing edges intact. Both iterations read the packed
// half-height surfaces.
vec3 smooth_normal(ivec2 pixel,vec3 p,vec3 n,bool narrow) {
    int num_dirs=narrow ? 6 : 4;
    int num_steps=narrow ? 3 : 6;
    float radius=narrow ? pc.scale.z : pc.scale.y;
    const vec2 sigma_n=cos(radians(vec2(45.0,30.0)));
    float angle=tau/float(num_dirs);
    vec2 rotator=vec2(cos(angle),sin(angle));
    vec2 kernel_dir=vec2(sin(angle+tau/12.0),cos(angle+tau/12.0));
    vec3 t_u,t_v;
    to_tangent(n,t_u,t_v);

    float wsum=0.001;
    vec3 mean_lean=vec3(0.0);
    vec3 u_x_lean=vec3(0.0);
    vec3 v_x_lean=vec3(0.0);
    vec2 uv_m1=vec2(0.0);
    vec3 uv_m2=vec3(0.0);
    vec2 center=vec2(pixel)+0.5;
    // Loops stay loops, as the original's [loop]. Directions are the same
    // screen directions for every pixel: the original's projected tangent
    // directions diverge between neighbouring pixels whose normals differ
    // (foliage, grass) and nearly doubled the cost there.
    [[dont_unroll]] for(int dir=0;dir<num_dirs;++dir) {
        vec2 sample_dir=kernel_dir;
        [[dont_unroll]] for(int stp=0;stp<num_steps;++stp) {
            float fi=float(stp+1)/float(num_steps);
            float r=narrow ? fi : fi*sqrt(fi);
            float pdf=narrow ? 1.0 : 1.5*sqrt(fi);
            vec2 position=center+sample_dir*(r*radius);
            if(any(lessThan(position,vec2(0.0))) || any(greaterThanEqual(position,vec2(pc.extent)))) break;
            ivec2 tap=ivec2(position);
            vec3 sp,sn;
            if(!packed_surface(ivec2(tap.x,tap.y/2),sp,sn)) break;

            float ndotn=dot(sn,n);
            vec3 lean=sn-n*ndotn;
            vec3 delta=sp-p;
            float u=dot(delta,t_u);
            float v=dot(delta,t_v);
            float h=dot(delta,n);
            float plane_distance=abs(h)+abs(dot(-delta,sn));

            float wn=clamp((ndotn-sigma_n.x)/(sigma_n.y-sigma_n.x),0.0,1.0);
            float wz=exp2(-plane_distance*plane_distance*10.0);
            float wd=exp2(-dot(delta,delta));
            float w=wn*wz*wd*pdf;

            mean_lean+=lean*w;
            u_x_lean+=u*lean*w;
            v_x_lean+=v*lean*w;
            uv_m1+=vec2(u,v)*w;
            uv_m2+=vec3(u*u,u*v,v*v)*w;
            wsum+=w;
            if(w<0.01) break;
        }
        kernel_dir=vec2(dot(kernel_dir,rotator),dot(kernel_dir,vec2(-rotator.y,rotator.x)));
    }

    uv_m1/=wsum;
    uv_m2/=wsum;
    u_x_lean/=wsum;
    v_x_lean/=wsum;
    mean_lean/=wsum;
    // Centred moment matrix, normalized by its determinant.
    vec3 cuv_m2=uv_m2-uv_m1.xxy*uv_m1.xyy;
    cuv_m2.xz+=1.0e-5;
    float det=cuv_m2.x*cuv_m2.z-cuv_m2.y*cuv_m2.y;
    cuv_m2*=1.0/(abs(det)<1.0e-12 ? 1.0e-12 : det);
    vec3 c_u_x_lean=u_x_lean-uv_m1.x*mean_lean;
    vec3 c_v_x_lean=v_x_lean-uv_m1.y*mean_lean;
    // Solve for the lean gradients and evaluate the model at the centre.
    vec3 leangrad_u=cuv_m2.z*c_u_x_lean-cuv_m2.y*c_v_x_lean;
    vec3 leangrad_v=-cuv_m2.y*c_u_x_lean+cuv_m2.x*c_v_x_lean;
    vec3 smoothed=mean_lean-uv_m1.x*leangrad_u-uv_m1.y*leangrad_v+n;
    return smoothed*inversesqrt(max(dot(smoothed,smoothed),1.0e-8));
}
bool half_texel(out ivec2 texel) {
    texel=ivec2(gl_GlobalInvocationID.xy);
    return texel.x<int(pc.extent.x) && texel.y<int((pc.extent.y+1u)/2u);
}
)glsl";
} // namespace

std::string BuildLaunchpadGbufferShader() {
    std::string source = kCommon;
    source += R"glsl(
layout(set=0,binding=3,rg32ui) uniform writeonly uimage2D destination;
void main() {
    ivec2 texel;
    if(!half_texel(texel)) return;
    vec3 p,n;
    bool surface=native_surface(ivec2(texel.x,texel.y*2),p,n);
    imageStore(destination,texel,uvec4(surface ? pack_surface(n,p.z) : uvec2(0u),0u,0u));
}
)glsl";
    return source;
}

std::string BuildLaunchpadSmoothWideShader() {
    std::string source = kCommon;
    source += R"glsl(
layout(set=0,binding=3,rg32ui) uniform writeonly uimage2D destination;
void main() {
    ivec2 texel;
    if(!half_texel(texel)) return;
    vec3 p,n;
    if(!packed_surface(texel,p,n)) {
        imageStore(destination,texel,uvec4(0u));
        return;
    }
    vec3 smoothed=smooth_normal(ivec2(texel.x,texel.y*2),p,n,false);
    imageStore(destination,texel,uvec4(pack_surface(smoothed,p.z),0u,0u));
}
)glsl";
    return source;
}

std::string BuildLaunchpadSmoothNarrowShader() {
    std::string source = kCommon;
    source += R"glsl(
layout(set=0,binding=3,rgba16f) uniform writeonly image2D destination;
vec3 uv_position(vec2 uv,float z) {
    vec3 s=vec3(uv,1.0);
    return vec3(dot(pc.view_x.xyz,s),dot(pc.view_y.xyz,s),1.0)*z;
}
float albedo_mean(vec2 uv) {
    return textureLod(sampler2D(albedo,linear_sampler),uv,0.0).r;
}
// Surface relief from the albedo estimate: a weighted least-squares fit of its
// gradient over a golden-angle spiral in the tangent plane, subtracted from the
// normal as a height map's gradient is.
vec3 textured_normal(ivec2 pixel,vec3 p,vec3 n) {
    float center_luma=albedo_mean((vec2(pixel)+0.5)/vec2(pc.extent));
    // The screen-x neighbour two pixels away orients the tangent frame.
    ivec2 side=pixel.x+2<int(pc.extent.x) ? pixel+ivec2(2,0) : pixel-ivec2(2,0);
    float side_z=view_depth(side);
    vec3 e_x=p-proj_position(side,side_z<sky_z ? side_z/pc.scale.x : p.z);
    vec3 t_x=e_x-n*dot(n,e_x);
    float t_length2=dot(t_x,t_x);
    if(t_length2<1.0e-20) return n;
    t_x*=inversesqrt(t_length2);
    vec3 t_y=cross(n,t_x);
    vec3 m_parametric=vec3(0.0);
    vec2 m_height=vec2(0.0);
    int samples=int(pc.detail.z);
    // The spiral is laid out on screen, its step the original's tangent-plane
    // step over the pixel footprint at this depth: neighbouring pixels read
    // the same offsets, and the fit uses where each tap actually lies.
    float footprint=length(proj_position(pixel+ivec2(1,0),p.z)-proj_position(pixel,p.z));
    float step_pixels=pc.detail.x/max(footprint,1.0e-8);
    vec2 center=vec2(pixel)+0.5;
    for(int j=1;j<=samples;++j) {
        float angle=2.39996*float(j);
        vec2 position=center+vec2(cos(angle),sin(angle))*(step_pixels*float(j));
        vec2 tuv=position/vec2(pc.extent);
        ivec2 tap=ivec2(floor(position));
        if(any(lessThan(tap,ivec2(0))) || any(greaterThanEqual(tap,ivec2(pc.extent)))) continue;
        float z=view_depth(tap);
        if(z>=sky_z) continue;
        // Project the surface point actually seen there onto the tangent plane.
        vec3 delta=uv_position(tuv,z/pc.scale.x)-p;
        vec2 axis=vec2(dot(delta,t_x),dot(delta,t_y));
        float w=1.0/(0.05+abs(dot(n,delta)))/float(j*j);
        float h=albedo_mean(tuv)-center_luma;
        m_parametric+=axis.xxy*axis.xyy*w;
        m_height+=axis*h*w;
    }
    float det=m_parametric.x*m_parametric.z-m_parametric.y*m_parametric.y;
    vec2 grad=(m_parametric.zx*m_height.xy-m_parametric.yy*m_height.yx)/(det+1.0e-6);
    grad*=pc.detail.y;
    return normalize(n-grad.x*t_x-grad.y*t_y);
}
void main() {
    ivec2 pixel=ivec2(gl_GlobalInvocationID.xy);
    if(any(greaterThanEqual(pixel,ivec2(pc.extent)))) return;
    vec4 guide=texelFetch(sampler2D(view_normals,point_sampler),pixel,0);
    vec3 p,n;
    if(!native_surface(pixel,p,n)) {
        imageStore(destination,pixel,guide);
        return;
    }
    vec3 refined=(pc.flags&2u)!=0u ? n : smooth_normal(pixel,p,n,true);
    if((pc.flags&1u)!=0u) refined=textured_normal(pixel,p,refined);
    refined.z=-refined.z;
    imageStore(destination,pixel,vec4(refined*0.5+0.5,guide.a));
}
)glsl";
    return source;
}

} // namespace Fast::Immerse
