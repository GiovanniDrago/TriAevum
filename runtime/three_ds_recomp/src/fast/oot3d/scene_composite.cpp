#include "fast/oot3d/scene_composite.h"

#include "fast/oot3d/ambient_occlusion_composite.h"
#include "fast/oot3d/reflection_debug.h"
#include "fast/oot3d/toon_outline_shader.h"

namespace Fast::Oot3d {

std::string BuildSceneCompositeComputeShader() {
    std::string source = R"glsl(#version 450
layout(local_size_x=8,local_size_y=8,local_size_z=1) in;
layout(set=0,binding=0) uniform texture2D scene_color;
layout(set=0,binding=1) uniform texture2D ao_visibility;
layout(set=0,binding=2) uniform texture2D filtered_reflection;
layout(set=0,binding=3) uniform texture2D material_guide;
layout(set=0,binding=4) uniform texture2D normal_guide;
layout(set=0,binding=5) uniform texture2D depth_guide;
layout(set=0,binding=6) uniform texture2D ambient_guide;
layout(set=0,binding=7,rgba16f) uniform writeonly image2D composite_output;
layout(set=0,binding=8) uniform sampler composite_sampler;
layout(set=0,binding=9) uniform texture2D transparent_depth_guide;
layout(set=0,binding=10) uniform texture2D fog_guide;
layout(set=0,binding=11) uniform texture2D outline_geometry_guide;
layout(set=0,binding=12) uniform texture2D diffuse_illumination;
layout(set=0,binding=13) uniform texture2D diffuse_albedo_guide;
layout(push_constant) uniform CompositeState {
    uvec2 extent;
    uint ambient_occlusion;
    uint reflections;
    float reflection_strength;
    uint reflection_debug;
    uint outline;
    uint input_linear;
    vec2 inverse_size;
    float outline_width;
    float outline_depth_sensitivity;
    float outline_normal_sensitivity;
    float outline_softness;
    vec3 outline_color;
    float outline_opacity;
    uint global_illumination;
    float diffuse_intensity;
    uint global_illumination_debug;
    uint reserved;
    float ambient_r;
    float ambient_g;
    float ambient_b;
    float reference_radius;
    float additive;
    float bounce_strength;
    float occlusion_strength;
    float padding;
} state;

vec3 oot3d_gi_srgb_to_linear(vec3 value) {
    value=max(value,vec3(0.0));
    return mix(pow((value+0.055)/1.055,vec3(2.4)),value/12.92,lessThanEqual(value,vec3(0.04045)));
}
vec3 oot3d_gi_linear_to_srgb(vec3 value) {
    value=max(value,vec3(0.0));
    return mix(1.055*pow(value,vec3(1.0/2.4))-0.055,value*12.92,lessThanEqual(value,vec3(0.0031308)));
}
)glsl";
    source += AmbientOcclusionCompositeShaderLibrary();
    source += ReflectionDebugShaderLibrary();
    source += ToonOutlineComputeShaderLibrary();
    source += R"glsl(

void main(){
    ivec2 pixel=ivec2(gl_GlobalInvocationID.xy);
    if(any(greaterThanEqual(pixel,ivec2(state.extent)))) return;
    vec2 uv=(vec2(pixel)+0.5)/vec2(state.extent);
    vec4 color=texture(sampler2D(scene_color,composite_sampler),uv);
    if(state.ambient_occlusion!=0u) {
        float ao_value=texture(
            sampler2D(ao_visibility,composite_sampler),uv).r;
        float scene_coverage=texture(
            sampler2D(ambient_guide,composite_sampler),uv).a;
        color.rgb*=oot3d_scene_ambient_occlusion_visibility(
            ao_value,scene_coverage);
    }
    // Diffuse GI over OoT's baked lighting, where the albedo guide is valid
    // (coverage: zero on clear, HUD and replacing surfaces, the transmission
    // under overlays), attenuated by AO. The irradiance is traced with the
    // native sky as environment and compared with a reference: occlusion
    // scales the native colour by the irradiance ratio (never below black,
    // like AO), bounce adds albedo times the excess light.
    if(state.global_illumination!=0u) {
        vec4 albedo=texture(sampler2D(diffuse_albedo_guide,composite_sampler),uv);
        vec3 irradiance=texture(sampler2D(diffuse_illumination,composite_sampler),uv).rgb;
        float coverage=clamp(albedo.a,0.0,1.0);
        float visibility=state.ambient_occlusion!=0u
            ? clamp(texture(sampler2D(ao_visibility,composite_sampler),uv).r,0.0,1.0) : 1.0;
        // The native lighting is baked (vertex colours, textures) and already
        // holds the scene's average indirect light: the local reference is the
        // mean irradiance of neighbouring surfaces with a similar orientation
        // (a wall or a character is not compared with the open ground).
        vec3 center_normal=texture(sampler2D(normal_guide,composite_sampler),uv).xyz*2.0-1.0;
        center_normal*=inversesqrt(max(dot(center_normal,center_normal),1.0e-6));
        vec3 neighbourhood=vec3(0.0);
        float taps=0.0;
        // Three rings out to the reference radius (fraction of the height).
        for(int ring=1;ring<=3;++ring){
            vec2 radius=vec2(state.reference_radius*float(ring)/3.0)*
                vec2(float(state.extent.y)/float(state.extent.x),1.0);
            for(int k=0;k<8;++k){
                float angle=6.2831853*(float(k)+0.37*float(ring))/8.0;
                vec2 tap=clamp(uv+radius*vec2(cos(angle),sin(angle)),vec2(0.0),vec2(1.0));
                vec3 tap_normal=texture(sampler2D(normal_guide,composite_sampler),tap).xyz*2.0-1.0;
                tap_normal*=inversesqrt(max(dot(tap_normal,tap_normal),1.0e-6));
                float similar=max(dot(center_normal,tap_normal),0.0);
                similar*=similar;
                float valid=similar*similar*
                    texture(sampler2D(diffuse_albedo_guide,composite_sampler),tap).a;
                neighbourhood+=valid*max(texture(sampler2D(diffuse_illumination,composite_sampler),tap).rgb,vec3(0.0));
                taps+=valid;
            }
        }
        vec3 received=max(irradiance,vec3(0.0));
        vec3 reference=taps>0.5 ? neighbourhood/taps : received;
        // Additive 1: the reference is the open sky itself.
        vec3 sky=vec3(state.ambient_r,state.ambient_g,state.ambient_b);
        reference=mix(reference,sky,clamp(state.additive,0.0,1.0));
        const vec3 luma=vec3(0.2126,0.7152,0.0722);
        float ratio=clamp(dot(received,luma)/max(dot(reference,luma),1.0e-4),0.0,1.0);
        float occlusion=1.0-pow(ratio,max(state.occlusion_strength*state.diffuse_intensity,0.0));
        occlusion*=coverage;
        vec3 bounce=coverage*visibility*state.diffuse_intensity*state.bounce_strength*
            oot3d_gi_srgb_to_linear(albedo.rgb)*max(received-reference,vec3(0.0));
        vec3 native_linear=state.input_linear!=0u ? color.rgb : oot3d_gi_srgb_to_linear(color.rgb);
        vec3 removed=native_linear*occlusion;
        vec3 composed=native_linear-removed+bounce;
        color.rgb=state.input_linear!=0u ? composed : oot3d_gi_linear_to_srgb(composed);
        if(state.global_illumination_debug!=0u) {
            vec3 view=state.global_illumination_debug==1u ? irradiance
                : state.global_illumination_debug==2u ? albedo.rgb
                : state.global_illumination_debug==3u ? vec3(coverage)
                : state.global_illumination_debug==4u ? bounce
                : removed;
            color=vec4(state.input_linear!=0u || state.global_illumination_debug==2u
                ? view : oot3d_gi_linear_to_srgb(view),1.0);
        }
    }
    if(state.reflections!=0u){
        vec4 reflection=texture(sampler2D(filtered_reflection,composite_sampler),uv);
        vec4 material=texture(sampler2D(material_guide,composite_sampler),uv);
        if(state.reflection_debug!=0u)
            color.rgb=oot3d_reflection_debug_color(
                state.reflection_debug,material,reflection);
        else
            color.rgb=mix(color.rgb,reflection.rgb,
                clamp(reflection.a*state.reflection_strength,0.0,1.0));
    }
    if(state.outline!=0u){
        float edge=oot3d_toon_outline_edge(
            uv,
            state.inverse_size,state.outline_width,
            state.outline_depth_sensitivity,
            state.outline_normal_sensitivity,state.outline_softness)*
            state.outline_opacity;
        vec4 fog=texture(sampler2D(fog_guide,composite_sampler),uv);
        vec3 nativeTint=mix(fog.rgb,state.outline_color,fog.a);
        vec3 tint=state.input_linear!=0u
            ? oot3d_outline_srgb_to_linear(nativeTint) : nativeTint;
        color.rgb=mix(color.rgb,tint,edge);
    }
    imageStore(composite_output,pixel,color);
}
)glsl";
    return source;
}

} // namespace Fast::Oot3d
