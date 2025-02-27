#version 450 core

#extension GL_AMD_gpu_shader_half_float: enable

layout(binding = 0) buffer Buffers
{
    vec3  fv3;
    bvec3 bv3;
    ivec3 iv3;
};

layout(location = 0) out vec3 fragColor;

void main()
{
    f16vec3 f16v3_1 = f16vec3(fv3);
    f16vec3 f16v3_2 = f16vec3(fv3);
    float16_t f16 = float16_t(fv3.x);

    bvec2 bv2 = bvec2(false);

    f16v3_1 = abs(f16v3_1);
    f16v3_1 = sign(f16v3_1);
    f16v3_1 = floor(f16v3_1);
    f16v3_1 = trunc(f16v3_1);
    f16v3_1 = round(f16v3_1);
    f16v3_1 = roundEven(f16v3_1);
    f16v3_1 = ceil(f16v3_1);
    f16v3_1 = fract(f16v3_1);
    f16v3_1 = mod(f16v3_1, f16v3_2);
    f16v3_1 = modf(f16v3_1, f16v3_2);
    f16v3_1 = min(f16v3_1, f16v3_2);
    f16v3_1 = clamp(f16v3_1, f16v3_2.x, f16v3_2.y);
    f16v3_1 = mix(f16v3_1, f16v3_2, f16);
    f16v3_1 = mix(f16v3_1, f16v3_2, bv3);
    f16v3_1 = step(f16v3_1, f16v3_2);
    f16v3_1 = smoothstep(f16v3_2.x, f16v3_2.y, f16v3_1);
    bv2.x   = isnan(f16v3_1).x;
    bv2.y   = isinf(f16v3_2).x;
    f16v3_1 = fma(f16v3_1, f16v3_2, f16vec3(f16));
    f16v3_1 = frexp(f16v3_1, iv3);
    f16v3_1 = ldexp(f16v3_1, iv3);
    f16v3_1 = max(f16v3_1, f16v3_2);

    fragColor = any(bv2) ? f16v3_1 : f16v3_2;
}
// BEGIN_SHADERTEST
/*
; RUN: amdllpc -spvgen-dir=%spvgendir% -v %gfxip %s | FileCheck -check-prefix=SHADERTEST %s
; SHADERTEST-LABEL: {{^// LLPC}} SPIRV-to-LLVM translation results
; SHADERTEST-LABEL: {{^// LLPC}} SPIR-V lowering results
; SHADERTEST: = call reassoc nnan nsz arcp contract <3 x half> @llvm.fabs.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract <3 x half> (...) @llpc.call.fsign.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract <3 x half> @llvm.floor.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract <3 x half> @llvm.trunc.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract <3 x half> @llvm.rint.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract <3 x half> @llvm.ceil.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract <3 x half> (...) @llpc.call.fract.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract <3 x half> (...) @llpc.call.fmod.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract <3 x half> @llvm.trunc.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract <3 x half> (...) @llpc.call.fmin.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract <3 x half> (...) @llpc.call.fclamp.v3f16(<3 x half>
; SHADERTEST: = call <3 x half> @llvm.fmuladd.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract <3 x half> (...) @llpc.call.smooth.step.v3f16(<3 x half>
; SHADERTEST: = call <3 x i1> (...) @llpc.call.isnan.v3i1(<3 x half>
; SHADERTEST: = call <3 x i1> (...) @llpc.call.isinf.v3i1(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract <3 x half> (...) @llpc.call.fma.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract <3 x half> (...) @llpc.call.extract.significand.v3f16(<3 x half>
; SHADERTEST: = call <3 x i16> (...) @llpc.call.extract.exponent.v3i16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract <3 x half> (...) @llpc.call.ldexp.v3f16(<3 x half>
; SHADERTEST: = call reassoc nnan nsz arcp contract <3 x half> (...) @llpc.call.fmax.v3f16(<3 x half>
; SHADERTEST-LABEL: {{^// LLPC}} pipeline before-patching results
; SHADERTEST-LABEL: {{^// LLPC}} pipeline patching results
; SHADERTEST: AMDLLPC SUCCESS
*/
// END_SHADERTEST
