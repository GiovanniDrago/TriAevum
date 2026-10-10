# Rim light

The modern surface renderer adds a restrained edge response from the lights a
surface already evaluates. It adds no light sources, passes or shadow maps and
does not change native draw order, blending, alpha, depth or stencil.

`SceneEnvironmentRuntime` owns the settings, saved with the day/night scene
profile. The only controls are **F1 > Lighting > Lights & Shadows > Scene
lights > Rim light**: enable, intensity (0–8x), sharpness (1–16), ambient rim
(0–0.25), and Actors / Actors and world. Larger sharpness narrows the edge.
All evaluated lights participate; there is no second global override.

The built-in base environment enables
actors with intensity 0.35, sharpness 4 and ambient 0.05. Existing files without
the optional `rimLight` object retain their rim-off appearance. To update an
existing play profile, merge this object, preserving its other settings:

```json
"rimLight": {
  "enabled": true,
  "intensity": 0.35,
  "sharpness": 4,
  "ambient": 0.05,
  "scope": "actors"
}
```

The version-5 schema and runtime codec validate types, finite ranges, scope,
missing members and misspelled rim keys. Preview rejects invalid values before
publication. Save/reload, night profiles and hot reload use that same codec.
No files under `I:/_triaevum_work/perf-levers/play-kokiri*` are modified by this
implementation; Claude applies the object to those profiles.

For normalized shading normal `N`, surface-to-camera vector `V` and direction
toward a light `L`, the added light weight is:

```text
F = (1 - clamp(abs(dot(N,V)), 0, 1)) ^ sharpness
B = smoothstep(-0.25, 0.5, dot(L,-V))
W = clamp((dot(N,L) + 0.5) / 1.5, 0, 1)
rim_i = intensity * F * B * W * shadowed_radiance_i
```

The backlight gate rejects a camera-facing light and admits side/back lights;
the wrap allows an edge response beyond Lambert's terminator. Each multiplier
is bounded, with no division by a grazing cosine. This is an artistic addition,
not an energy-conserving BRDF: each light's added weight is at most intensity,
and many lights sum as ordinary illumination does. Attenuation, native gains,
spot cones, area form factors, source color and shadow visibility are reused,
so a fully shadowed source contributes no rim. Fill and placed Sun remain
unshadowed because their existing direct contributions are unshadowed. A light
without an allocated shadow retains the existing renderer's visibility policy;
the rim does not create shadow coverage that its light lacks.

The plain per-pixel path applies the term before native tint/TEV composition.
PBR applies it in linear lighting, weighted by albedo, using the final mapped
and seam-adjusted normal. The optional ambient term is `ambient * F` times
resolved ambient color/radiance; PBR also respects material AO. It has no
directional shadow and can remain visible in an interior or key shadow. Set
Ambient rim to zero when checking direct-shadow behavior. Rims are excluded
from GI source radiance: moving the camera must not create bounce lighting.

The OOT3D adapter admits proven opaque world draws using the existing native
actor attribution, independently of skinning or texture. Actors are the default;
static architecture requires Actors and world. UI, screen content, atmosphere,
unknown layers, transparent/additive blends, projected shadows, authored unlit
appearance and isolated lighting are excluded. Alpha-tested opaque geometry
keeps its original alpha test. Fade passes are intentionally excluded.

Both the plain per-pixel surface path and PerPixelPBR's material path implement
the response. Scene profiles remain a Modern-only feature: Native/NativePBR,
Authentic, F2 and disabling Use scene environment bypass the rim and retain its
saved preference. Disabling rim canonicalizes its renderer settings and zeroes
the uniform; changing disabled knobs cannot change rendering inputs or histories.
The uniform controls the existing shader, with no rim shader/pipeline variant,
extra resource, effect decision or blend-state change.

Verification uses the environment owner tests, adapter/surface-uniform tests,
the actual ImGui F1 widget smoke, and a Vulkan equation test executing the
production surface evaluator. The GPU test covers all source kinds, off,
front/back/grazing, visibility, range, ambient, invalid normals and GI isolation;
it also compiles the complete PBR shader bodies with and without native fragment
lighting. Game appearance and performance remain the lab's responsibility.

Verified on 2026-10-10: `TriAevum.exe` and all affected targets build with
`build-host.cmd`; environment tests pass 69 checks, pipeline tests pass 54/54,
graphics foundation passes 352/352, and the RTX 3060 passes 66 GPU cases plus
both PBR shader compilations. `triaevum_f1_settings_smoke --scene-environment`
passes the actual scene/rim widgets. The full F1 suite stops earlier in the
unchanged material-editor test at "creation used the catalog hash instead of
the native texture's Azahar identity"; it does not reach the scene panel.
The scene-0000 template passes the environment tool's validation and canonical
format check. No game was run.

Claude's lab should use the host-build savestates: compare fixed-frame off/on
views in Kokiri, Deku, Zora, Shadow Temple, Temple of Time and a house. Rotate
the camera and move an actor through key and local-light shadows; first set
ambient rim to zero, then restore 0.05. Check normal-mapped actors, sharpness
1/4/16, stacked local lights, actor fade passes, a rigid prop, UI/particles and
Actors versus Actors and world. Verify F2 restoration, scene/night transitions,
save/reload, unchanged alpha/depth/stencil and no new passes or pipeline churn.
Record GPU time and watch for excessively bright edges at maximum intensity.
