/***************************************************************************
Copyright (c) 2024, The OpenBLAS Project
All rights reserved.
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:
1. Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in
the documentation and/or other materials provided with the
distribution.
3. Neither the name of the OpenBLAS project nor the names of
its contributors may be used to endorse or promote products
derived from this software without specific prior written permission.
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE OPENBLAS PROJECT OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*****************************************************************************/

#include "common.h"

#include <arm_neon.h>
#include <arm_sve.h>
#ifdef __ARM_NEON_SVE_BRIDGE
#include <arm_neon_sve_bridge.h>
#else
#define svdup_neonq_f32(fixed_reg)                                             \
  ({                                                                           \
    svfloat32_t scalable_reg;                                                  \
    asm("mov %0.q, %q1" : "=w"(scalable_reg) : "w"(fixed_reg) :);              \
    scalable_reg;                                                              \
  })
#define svdup_neonq_f64(fixed_reg)                                             \
  ({                                                                           \
    svfloat64_t scalable_reg;                                                  \
    asm("mov %0.q, %q1" : "=w"(scalable_reg) : "w"(fixed_reg) :);              \
    scalable_reg;                                                              \
  })
#endif

#define A_ELEMENT_K(m, offset_k) A[(i + (m)) + (k + offset_k) * lda]
#define A_ELEMENT(m) A_ELEMENT_K(m, 0)

#define B_ELEMENT_K(n, offset_k) B[(k + offset_k) + (j + (n)) * ldb]
#define B_ELEMENT(n) B_ELEMENT_K(n, 0)

#define C_ELEMENT(m, n) C[(i + (m)) + (j + (n)) * ldc]

#define PACK_ELEMENT_K(n, offset_k) packed_b[(k + offset_k) * 4 + n]
#define PACK_ELEMENT(n) PACK_ELEMENT_K(n, 0)

// ASIMD
#define DECLARE_RESULT_VECTOR2(m, n)                                           \
  float64x2_t result##m##n = vdupq_n_f64(0.0);
#define DECLARE_RESULT(m, n) float64_t result##m##n = 0.0;
#define BROADCAST_LOAD_A2(m, offset_k)                                         \
  float64x2_t a##m##_k##offset_k = vld1q_dup_f64(&A_ELEMENT_K(m, offset_k));
#define LOAD_A1(m, offset_k)                                                   \
  float64_t a##m##_k##offset_k = A_ELEMENT_K(m, offset_k);
#define VECTOR_LOAD_B_K2(n, offset_k)                                          \
  float64x2_t b##k##n##_k##offset_k = vld1q_f64(&B_ELEMENT_K(n, offset_k));
#define TRANSPOSE_B2_K2(n0, n1, offset_k0, offset_k1)                          \
  float64x2_t b##n0##_k##offset_k0 =                                           \
    vzip1q_f64(b##k##n0##_k##offset_k0, b##k##n1##_k##offset_k0);              \
  float64x2_t b##n0##_k##offset_k1 =                                           \
    vzip2q_f64(b##k##n0##_k##offset_k0, b##k##n1##_k##offset_k0);

#define SCALE_B2_K2(n0, offset_k0, offset_k1)                                  \
  svfloat64_t b##s##n0##_k##offset_k0 = svdup_neonq_f64(b##n0##_k##offset_k0); \
  svfloat64_t b##s##n0##_k##offset_k1 = svdup_neonq_f64(b##n0##_k##offset_k1);
#define GATHER_LOAD_B2(n, offset_k)                                            \
  float64x2_t b##n##_k##offset_k = vdupq_n_f64(B_ELEMENT_K(n, offset_k));      \
  b##n##_k##offset_k =                                                         \
    vsetq_lane_f64(B_ELEMENT_K(n + 1, offset_k), b##n##_k##offset_k, 1);
#define VECTOR_UNPACK_B2(n, offset_k)                                          \
  float64x2_t b##n##_k##offset_k = vld1q_f64(&PACK_ELEMENT_K(n, offset_k));
#define VECTOR_PACK_B2(n, offset_k)                                            \
  vst1q_f64(&PACK_ELEMENT_K(n, offset_k), b##n##_k##offset_k);
#define PACK_B0(n, offset_k)                                                   \
  PACK_ELEMENT_K(n, offset_k) = vget_lane_f64(b##n##_k##offset_k, 0);
#define UPDATE_RESULT_VECTOR2(m, n, offset_k)                                  \
  result##m##n =                                                               \
    vfmaq_f64(result##m##n, a##m##_k##offset_k, b##n##_k##offset_k);
#define UPDATE_RESULT(m, n, offset_k)                                          \
  result##m##n = result##m##n + a##m##_k##offset_k * b##n##_k##offset_k;
#ifdef B0
#define SCATTER_STORE2(m, n)                                                   \
  result##m##n = vmulq_f64(result##m##n, vdupq_n_f64(alpha));                  \
  C_ELEMENT(m, n + 0) = vgetq_lane_f64(result##m##n, 0);                       \
  C_ELEMENT(m, n + 1) = vgetq_lane_f64(result##m##n, 1);
#else
#define SCATTER_STORE2(m, n)                                                   \
  result##m##n = vmulq_f64(result##m##n, vdupq_n_f64(alpha));                  \
  C_ELEMENT(m, n + 0) =                                                        \
    C_ELEMENT(m, n + 0) * beta + vgetq_lane_f64(result##m##n, 0);              \
  C_ELEMENT(m, n + 1) =                                                        \
    C_ELEMENT(m, n + 1) * beta + vgetq_lane_f64(result##m##n, 1);
#endif

// SVE
#define DECLARE_RESULT_VECTOR(m, n) svfloat64_t result##m##n = svdup_f64(0.0);
#define BROADCAST_LOAD_A(m, offset_k)                                          \
  svfloat64_t a##s##m##_k##offset_k = svdup_f64(A_ELEMENT_K(m, offset_k));
#define BROADCAST_LOAD_B(n, offset_k)                                          \
  svfloat64_t b##s##n##_k##offset_k = svdup_f64(B_ELEMENT_K(n, offset_k));
#define VECTOR_LOAD_A(pg, m, offset_k)                                         \
  svfloat64_t a##s##m##_k##offset_k =                                          \
    svld1(pg, &A_ELEMENT_K(v_size * m, offset_k));
#define QUADWORD_LOAD_B(n, offset_k)                                           \
  svfloat64_t b##s##n##_k##offset_k =                                          \
    svld1rq(pg_true, &B_ELEMENT_K(n, offset_k));
#define PACK_B(n, offset_k)                                                    \
  svst1(pg_first, &PACK_ELEMENT_K(n, offset_k), b##s##n##_k##offset_k);
#define VECTOR_PACK_B(n, offset_k)                                             \
  svst1(pg_true, &PACK_ELEMENT_K(n* v_size, offset_k), b##s##n##_k##offset_k);
#define QUADWORD_PACK_B(n, offset_k)                                           \
  svst1(pg_quad, &PACK_ELEMENT_K(n, offset_k), b##s##n##_k##offset_k);
#define UNPACK_VECTOR_B(n, offset_k)                                           \
  svfloat64_t b##s##n##_k##offset_k =                                          \
    svld1(pg_true, &PACK_ELEMENT_K(n * v_size, offset_k));
#define UNPACK_BROADCAST_B(n, offset_k)                                        \
  svfloat64_t b##s##n##_k##offset_k = svdup_f64(PACK_ELEMENT_K(n, offset_k));
#define UNPACK_QUADWORD_B(n, offset_k)                                         \
  svfloat64_t b##s##n##_k##offset_k =                                          \
    svld1rq(pg_true, &PACK_ELEMENT_K(n, offset_k));
#define UPDATE_RESULT_VECTOR(pg, m, n, offset_k)                               \
  result##m##n =                                                               \
    svmla_m(pg, result##m##n, a##s##m##_k##offset_k, b##s##n##_k##offset_k);
#define UPDATE_RESULT_VECTOR_QUADWORD(m, n, outer, lane, offset_k)             \
  result##m##n = svmla_lane(                                                   \
    result##m##n, a##s##m##_k##offset_k, b##s##outer##_k##offset_k, lane);
#ifdef B0
#define VECTOR_STORE(pg, m, n)                                                 \
  result##m##n = svmul_m(pg, result##m##n, alpha_vec);                         \
  svst1(pg, &C_ELEMENT(v_size* m, n), result##m##n);
#define SCATTER_STORE(pg, m, n)                                                \
  result##m##n = svmul_m(pg, result##m##n, alpha_vec);                         \
  svst1_scatter_index(                                                         \
    pg, &C_ELEMENT(v_size* m, n), svindex_u64(0LL, ldc), result##m##n);
#else
#define VECTOR_STORE(pg, m, n)                                                 \
  result##m##n = svmul_m(pg, result##m##n, alpha_vec);                         \
  result##m##n =                                                               \
    svmla_m(pg, result##m##n, svld1(pg, &C_ELEMENT(v_size * m, n)), beta_vec); \
  svst1(pg, &C_ELEMENT(v_size* m, n), result##m##n);
#define SCATTER_STORE(pg, m, n)                                                \
  result##m##n = svmul_m(pg, result##m##n, alpha_vec);                         \
  result##m##n = svmla_m(                                                      \
    pg,                                                                        \
    result##m##n,                                                              \
    svld1_gather_index(pg, &C_ELEMENT(v_size * m, n), svindex_u64(0LL, ldc)),  \
    beta_vec);                                                                 \
  svst1_scatter_index(                                                         \
    pg, &C_ELEMENT(v_size* m, n), svindex_u64(0LL, ldc), result##m##n);
#endif

#ifndef LIKELY
#ifdef __GNUC__
#define LIKELY(x) __builtin_expect(!!(x), 1)
#else
#define LIKELY(x) (x)
#endif
#endif
#ifndef UNLIKELY
#ifdef __GNUC__
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define UNLIKELY(x) (x)
#endif
#endif

#ifdef B0
int
CNAME(BLASLONG M,
      BLASLONG N,
      BLASLONG K,
      IFLOAT* A,
      BLASLONG lda,
      FLOAT alpha,
      IFLOAT* B,
      BLASLONG ldb,
      FLOAT* C,
      BLASLONG ldc)
#else
int
CNAME(BLASLONG M,
      BLASLONG N,
      BLASLONG K,
      IFLOAT* A,
      BLASLONG lda,
      FLOAT alpha,
      IFLOAT* B,
      BLASLONG ldb,
      FLOAT beta,
      FLOAT* C,
      BLASLONG ldc)
#endif
{
  const uint64_t v_size = svcntd();
  const uint64_t v_size2 = v_size * 2;
  const svbool_t pg_true = svptrue_b64();
  const svbool_t pg_quad = svwhilelt_b64(0, 2);
  const svbool_t pg_first = svwhilelt_b64(0, 1);
  const svfloat64_t alpha_vec = svdup_f64(alpha);
#ifndef B0
  const svfloat64_t beta_vec = svdup_f64(beta);
#endif
  const BLASLONG n4 = N & -4;
  const BLASLONG n2 = N & -2;
  const BLASLONG v_m2 = M & -v_size2;
  const BLASLONG v_m1 = M & -v_size;
  const BLASLONG k2 = K & -2;

  const int pack_b = M >= v_size2 && N >= 8 && K >= 8 ? 1 : 0;
  FLOAT* packed_b =
    (pack_b) ? packed_b = (FLOAT*)malloc(K * 4 * sizeof(FLOAT)) : NULL;

  BLASLONG j = 0;
  for (; j < n4; j += 4) {

    BLASLONG i = 0;
    for (; i < v_m2; i += v_size2) {

      BLASLONG k = 0;
      DECLARE_RESULT_VECTOR(0, 0);
      DECLARE_RESULT_VECTOR(0, 1);
      DECLARE_RESULT_VECTOR(0, 2);
      DECLARE_RESULT_VECTOR(0, 3);
      DECLARE_RESULT_VECTOR(1, 0);
      DECLARE_RESULT_VECTOR(1, 1);
      DECLARE_RESULT_VECTOR(1, 2);
      DECLARE_RESULT_VECTOR(1, 3);

      if (LIKELY(packed_b != NULL)) {
        if (i == 0) {
          for (; k < k2; k += 2) {

            VECTOR_LOAD_B_K2(0, 0);
            VECTOR_LOAD_B_K2(1, 0);
            TRANSPOSE_B2_K2(0, 1, 0, 1);
            SCALE_B2_K2(0, 0, 1);
            VECTOR_PACK_B2(0, 0);
            VECTOR_PACK_B2(0, 1);
            VECTOR_LOAD_A(pg_true, 0, 0);
            UPDATE_RESULT_VECTOR_QUADWORD(0, 0, 0, 0, 0);
            UPDATE_RESULT_VECTOR_QUADWORD(0, 1, 0, 1, 0);
            VECTOR_LOAD_A(pg_true, 0, 1);
            UPDATE_RESULT_VECTOR_QUADWORD(0, 0, 0, 0, 1);
            UPDATE_RESULT_VECTOR_QUADWORD(0, 1, 0, 1, 1);
            VECTOR_LOAD_B_K2(2, 0);
            VECTOR_LOAD_B_K2(3, 0);
            TRANSPOSE_B2_K2(2, 3, 0, 1);
            SCALE_B2_K2(2, 0, 1);
            VECTOR_PACK_B2(2, 0);
            VECTOR_PACK_B2(2, 1);
            UPDATE_RESULT_VECTOR_QUADWORD(0, 2, 2, 0, 0);
            UPDATE_RESULT_VECTOR_QUADWORD(0, 3, 2, 1, 0);
            UPDATE_RESULT_VECTOR_QUADWORD(0, 2, 2, 0, 1);
            UPDATE_RESULT_VECTOR_QUADWORD(0, 3, 2, 1, 1);
            VECTOR_LOAD_A(pg_true, 1, 0);
            UPDATE_RESULT_VECTOR_QUADWORD(1, 0, 0, 0, 0);
            UPDATE_RESULT_VECTOR_QUADWORD(1, 1, 0, 1, 0);
            UPDATE_RESULT_VECTOR_QUADWORD(1, 2, 2, 0, 0);
            UPDATE_RESULT_VECTOR_QUADWORD(1, 3, 2, 1, 0);
            VECTOR_LOAD_A(pg_true, 1, 1);
            UPDATE_RESULT_VECTOR_QUADWORD(1, 0, 0, 0, 1);
            UPDATE_RESULT_VECTOR_QUADWORD(1, 1, 0, 1, 1);
            UPDATE_RESULT_VECTOR_QUADWORD(1, 2, 2, 0, 1);
            UPDATE_RESULT_VECTOR_QUADWORD(1, 3, 2, 1, 1);
          }
          for (; k < K; k++) {

            BROADCAST_LOAD_B(0, 0);
            PACK_B(0, 0);
            VECTOR_LOAD_A(pg_true, 0, 0);
            UPDATE_RESULT_VECTOR(pg_true, 0, 0, 0);
            BROADCAST_LOAD_B(1, 0);
            PACK_B(1, 0);
            UPDATE_RESULT_VECTOR(pg_true, 0, 1, 0);
            VECTOR_LOAD_A(pg_true, 1, 0);
            UPDATE_RESULT_VECTOR(pg_true, 1, 0, 0);
            UPDATE_RESULT_VECTOR(pg_true, 1, 1, 0);
            BROADCAST_LOAD_B(2, 0);
            PACK_B(2, 0);
            UPDATE_RESULT_VECTOR(pg_true, 0, 2, 0);
            UPDATE_RESULT_VECTOR(pg_true, 1, 2, 0);
            BROADCAST_LOAD_B(3, 0);
            PACK_B(3, 0);
            UPDATE_RESULT_VECTOR(pg_true, 0, 3, 0);
            UPDATE_RESULT_VECTOR(pg_true, 1, 3, 0);
          }
        } else {
          for (; k < K; k++) {

            UNPACK_QUADWORD_B(0, 0);
            VECTOR_LOAD_A(pg_true, 0, 0);
            UPDATE_RESULT_VECTOR_QUADWORD(0, 0, 0, 0, 0);
            UPDATE_RESULT_VECTOR_QUADWORD(0, 1, 0, 1, 0);
            UNPACK_QUADWORD_B(2, 0);
            UPDATE_RESULT_VECTOR_QUADWORD(0, 2, 2, 0, 0);
            UPDATE_RESULT_VECTOR_QUADWORD(0, 3, 2, 1, 0);
            VECTOR_LOAD_A(pg_true, 1, 0);
            UPDATE_RESULT_VECTOR_QUADWORD(1, 0, 0, 0, 0);
            UPDATE_RESULT_VECTOR_QUADWORD(1, 1, 0, 1, 0);
            UPDATE_RESULT_VECTOR_QUADWORD(1, 2, 2, 0, 0);
            UPDATE_RESULT_VECTOR_QUADWORD(1, 3, 2, 1, 0);
          }
        }
      } else {
        for (; k < k2; k += 2) {

          VECTOR_LOAD_B_K2(0, 0);
          VECTOR_LOAD_B_K2(1, 0);
          TRANSPOSE_B2_K2(0, 1, 0, 1);
          SCALE_B2_K2(0, 0, 1);
          VECTOR_LOAD_A(pg_true, 0, 0);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 0, 0, 0, 0);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 1, 0, 1, 0);
          VECTOR_LOAD_A(pg_true, 0, 1);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 0, 0, 0, 1);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 1, 0, 1, 1);
          VECTOR_LOAD_B_K2(2, 0);
          VECTOR_LOAD_B_K2(3, 0);
          TRANSPOSE_B2_K2(2, 3, 0, 1);
          SCALE_B2_K2(2, 0, 1);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 2, 2, 0, 0);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 3, 2, 1, 0);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 2, 2, 0, 1);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 3, 2, 1, 1);
          VECTOR_LOAD_A(pg_true, 1, 0);
          UPDATE_RESULT_VECTOR_QUADWORD(1, 0, 0, 0, 0);
          UPDATE_RESULT_VECTOR_QUADWORD(1, 1, 0, 1, 0);
          UPDATE_RESULT_VECTOR_QUADWORD(1, 2, 2, 0, 0);
          UPDATE_RESULT_VECTOR_QUADWORD(1, 3, 2, 1, 0);
          VECTOR_LOAD_A(pg_true, 1, 1);
          UPDATE_RESULT_VECTOR_QUADWORD(1, 0, 0, 0, 1);
          UPDATE_RESULT_VECTOR_QUADWORD(1, 1, 0, 1, 1);
          UPDATE_RESULT_VECTOR_QUADWORD(1, 2, 2, 0, 1);
          UPDATE_RESULT_VECTOR_QUADWORD(1, 3, 2, 1, 1);
        }
        for (; k < K; k++) {

          BROADCAST_LOAD_B(0, 0);
          VECTOR_LOAD_A(pg_true, 0, 0);
          UPDATE_RESULT_VECTOR(pg_true, 0, 0, 0);
          BROADCAST_LOAD_B(1, 0);
          UPDATE_RESULT_VECTOR(pg_true, 0, 1, 0);
          VECTOR_LOAD_A(pg_true, 1, 0);
          UPDATE_RESULT_VECTOR(pg_true, 1, 0, 0);
          UPDATE_RESULT_VECTOR(pg_true, 1, 1, 0);
          BROADCAST_LOAD_B(2, 0);
          UPDATE_RESULT_VECTOR(pg_true, 0, 2, 0);
          UPDATE_RESULT_VECTOR(pg_true, 1, 2, 0);
          BROADCAST_LOAD_B(3, 0);
          UPDATE_RESULT_VECTOR(pg_true, 0, 3, 0);
          UPDATE_RESULT_VECTOR(pg_true, 1, 3, 0);
        }
      }
      VECTOR_STORE(pg_true, 0, 0);
      VECTOR_STORE(pg_true, 0, 1);
      VECTOR_STORE(pg_true, 0, 2);
      VECTOR_STORE(pg_true, 0, 3);
      VECTOR_STORE(pg_true, 1, 0);
      VECTOR_STORE(pg_true, 1, 1);
      VECTOR_STORE(pg_true, 1, 2);
      VECTOR_STORE(pg_true, 1, 3);
    }
    for (; i < v_m1; i += v_size) {

      BLASLONG k = 0;
      DECLARE_RESULT_VECTOR(0, 0);
      DECLARE_RESULT_VECTOR(0, 1);
      DECLARE_RESULT_VECTOR(0, 2);
      DECLARE_RESULT_VECTOR(0, 3);

      if (LIKELY(packed_b != NULL)) {
        for (; k < K; k++) {

          UNPACK_QUADWORD_B(0, 0);
          VECTOR_LOAD_A(pg_true, 0, 0);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 0, 0, 0, 0);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 1, 0, 1, 0);
          UNPACK_QUADWORD_B(2, 0);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 2, 2, 0, 0);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 3, 2, 1, 0);
        }
      } else {
        for (; k < k2; k += 2) {

          VECTOR_LOAD_B_K2(0, 0);
          VECTOR_LOAD_B_K2(1, 0);
          TRANSPOSE_B2_K2(0, 1, 0, 1);
          SCALE_B2_K2(0, 0, 1);
          VECTOR_LOAD_A(pg_true, 0, 0);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 0, 0, 0, 0);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 1, 0, 1, 0);
          VECTOR_LOAD_A(pg_true, 0, 1);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 0, 0, 0, 1);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 1, 0, 1, 1);
          VECTOR_LOAD_B_K2(2, 0);
          VECTOR_LOAD_B_K2(3, 0);
          TRANSPOSE_B2_K2(2, 3, 0, 1);
          SCALE_B2_K2(2, 0, 1);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 2, 2, 0, 0);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 3, 2, 1, 0);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 2, 2, 0, 1);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 3, 2, 1, 1);
        }
        for (; k < K; k++) {

          BROADCAST_LOAD_B(0, 0);
          VECTOR_LOAD_A(pg_true, 0, 0);
          UPDATE_RESULT_VECTOR(pg_true, 0, 0, 0);
          BROADCAST_LOAD_B(1, 0);
          UPDATE_RESULT_VECTOR(pg_true, 0, 1, 0);
          BROADCAST_LOAD_B(2, 0);
          UPDATE_RESULT_VECTOR(pg_true, 0, 2, 0);
          BROADCAST_LOAD_B(3, 0);
          UPDATE_RESULT_VECTOR(pg_true, 0, 3, 0);
        }
      }
      VECTOR_STORE(pg_true, 0, 0);
      VECTOR_STORE(pg_true, 0, 1);
      VECTOR_STORE(pg_true, 0, 2);
      VECTOR_STORE(pg_true, 0, 3);
    }
    for (; i < M; i += v_size) {
      const svbool_t pg_tail = svwhilelt_b64((uint64_t)i, (uint64_t)(M));

      BLASLONG k = 0;
      DECLARE_RESULT_VECTOR(0, 0);
      DECLARE_RESULT_VECTOR(0, 1);
      DECLARE_RESULT_VECTOR(0, 2);
      DECLARE_RESULT_VECTOR(0, 3);

      if (LIKELY(packed_b != NULL)) {
        for (; k < K; k++) {

          UNPACK_QUADWORD_B(0, 0);
          VECTOR_LOAD_A(pg_tail, 0, 0);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 0, 0, 0, 0);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 1, 0, 1, 0);
          UNPACK_QUADWORD_B(2, 0);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 2, 2, 0, 0);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 3, 2, 1, 0);
        }
      } else {
        for (; k < k2; k += 2) {

          VECTOR_LOAD_B_K2(0, 0);
          VECTOR_LOAD_B_K2(1, 0);
          TRANSPOSE_B2_K2(0, 1, 0, 1);
          SCALE_B2_K2(0, 0, 1);
          VECTOR_LOAD_A(pg_tail, 0, 0);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 0, 0, 0, 0);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 1, 0, 1, 0);
          VECTOR_LOAD_A(pg_tail, 0, 1);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 0, 0, 0, 1);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 1, 0, 1, 1);
          VECTOR_LOAD_B_K2(2, 0);
          VECTOR_LOAD_B_K2(3, 0);
          TRANSPOSE_B2_K2(2, 3, 0, 1);
          SCALE_B2_K2(2, 0, 1);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 2, 2, 0, 0);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 3, 2, 1, 0);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 2, 2, 0, 1);
          UPDATE_RESULT_VECTOR_QUADWORD(0, 3, 2, 1, 1);
        }
        for (; k < K; k++) {

          BROADCAST_LOAD_B(0, 0);
          VECTOR_LOAD_A(pg_tail, 0, 0);
          UPDATE_RESULT_VECTOR(pg_tail, 0, 0, 0);
          BROADCAST_LOAD_B(1, 0);
          UPDATE_RESULT_VECTOR(pg_tail, 0, 1, 0);
          BROADCAST_LOAD_B(2, 0);
          UPDATE_RESULT_VECTOR(pg_tail, 0, 2, 0);
          BROADCAST_LOAD_B(3, 0);
          UPDATE_RESULT_VECTOR(pg_tail, 0, 3, 0);
        }
      }
      VECTOR_STORE(pg_tail, 0, 0);
      VECTOR_STORE(pg_tail, 0, 1);
      VECTOR_STORE(pg_tail, 0, 2);
      VECTOR_STORE(pg_tail, 0, 3);
    }
  }
  for (; j < n2; j += 2) {

    BLASLONG i = 0;
    for (; i < v_m2; i += v_size2) {

      BLASLONG k = 0;
      DECLARE_RESULT_VECTOR(0, 0);
      DECLARE_RESULT_VECTOR(0, 1);
      DECLARE_RESULT_VECTOR(1, 0);
      DECLARE_RESULT_VECTOR(1, 1);

      for (; k < k2; k += 2) {

        VECTOR_LOAD_B_K2(0, 0);
        VECTOR_LOAD_B_K2(1, 0);
        TRANSPOSE_B2_K2(0, 1, 0, 1);
        SCALE_B2_K2(0, 0, 1);
        VECTOR_LOAD_A(pg_true, 0, 0);
        UPDATE_RESULT_VECTOR_QUADWORD(0, 0, 0, 0, 0);
        UPDATE_RESULT_VECTOR_QUADWORD(0, 1, 0, 1, 0);
        VECTOR_LOAD_A(pg_true, 0, 1);
        UPDATE_RESULT_VECTOR_QUADWORD(0, 0, 0, 0, 1);
        UPDATE_RESULT_VECTOR_QUADWORD(0, 1, 0, 1, 1);
        VECTOR_LOAD_A(pg_true, 1, 0);
        UPDATE_RESULT_VECTOR_QUADWORD(1, 0, 0, 0, 0);
        UPDATE_RESULT_VECTOR_QUADWORD(1, 1, 0, 1, 0);
        VECTOR_LOAD_A(pg_true, 1, 1);
        UPDATE_RESULT_VECTOR_QUADWORD(1, 0, 0, 0, 1);
        UPDATE_RESULT_VECTOR_QUADWORD(1, 1, 0, 1, 1);
      }
      for (; k < K; k++) {

        BROADCAST_LOAD_B(0, 0);
        VECTOR_LOAD_A(pg_true, 0, 0);
        UPDATE_RESULT_VECTOR(pg_true, 0, 0, 0);
        BROADCAST_LOAD_B(1, 0);
        UPDATE_RESULT_VECTOR(pg_true, 0, 1, 0);
        VECTOR_LOAD_A(pg_true, 1, 0);
        UPDATE_RESULT_VECTOR(pg_true, 1, 0, 0);
        UPDATE_RESULT_VECTOR(pg_true, 1, 1, 0);
      }
      VECTOR_STORE(pg_true, 0, 0);
      VECTOR_STORE(pg_true, 0, 1);
      VECTOR_STORE(pg_true, 1, 0);
      VECTOR_STORE(pg_true, 1, 1);
    }
    for (; i < v_m1; i += v_size) {

      BLASLONG k = 0;
      DECLARE_RESULT_VECTOR(0, 0);
      DECLARE_RESULT_VECTOR(0, 1);

      for (; k < k2; k += 2) {

        VECTOR_LOAD_B_K2(0, 0);
        VECTOR_LOAD_B_K2(1, 0);
        TRANSPOSE_B2_K2(0, 1, 0, 1);
        SCALE_B2_K2(0, 0, 1);
        VECTOR_LOAD_A(pg_true, 0, 0);
        UPDATE_RESULT_VECTOR_QUADWORD(0, 0, 0, 0, 0);
        UPDATE_RESULT_VECTOR_QUADWORD(0, 1, 0, 1, 0);
        VECTOR_LOAD_A(pg_true, 0, 1);
        UPDATE_RESULT_VECTOR_QUADWORD(0, 0, 0, 0, 1);
        UPDATE_RESULT_VECTOR_QUADWORD(0, 1, 0, 1, 1);
      }
      for (; k < K; k++) {

        BROADCAST_LOAD_B(0, 0);
        VECTOR_LOAD_A(pg_true, 0, 0);
        UPDATE_RESULT_VECTOR(pg_true, 0, 0, 0);
        BROADCAST_LOAD_B(1, 0);
        UPDATE_RESULT_VECTOR(pg_true, 0, 1, 0);
      }
      VECTOR_STORE(pg_true, 0, 0);
      VECTOR_STORE(pg_true, 0, 1);
    }
    for (; i < M; i += v_size) {
      const svbool_t pg_tail = svwhilelt_b64((uint64_t)i, (uint64_t)(M));

      BLASLONG k = 0;
      DECLARE_RESULT_VECTOR(0, 0);
      DECLARE_RESULT_VECTOR(0, 1);

      for (; k < k2; k += 2) {

        VECTOR_LOAD_B_K2(0, 0);
        VECTOR_LOAD_B_K2(1, 0);
        TRANSPOSE_B2_K2(0, 1, 0, 1);
        SCALE_B2_K2(0, 0, 1);
        VECTOR_LOAD_A(pg_tail, 0, 0);
        UPDATE_RESULT_VECTOR_QUADWORD(0, 0, 0, 0, 0);
        UPDATE_RESULT_VECTOR_QUADWORD(0, 1, 0, 1, 0);
        VECTOR_LOAD_A(pg_tail, 0, 1);
        UPDATE_RESULT_VECTOR_QUADWORD(0, 0, 0, 0, 1);
        UPDATE_RESULT_VECTOR_QUADWORD(0, 1, 0, 1, 1);
      }
      for (; k < K; k++) {

        BROADCAST_LOAD_B(0, 0);
        VECTOR_LOAD_A(pg_tail, 0, 0);
        UPDATE_RESULT_VECTOR(pg_tail, 0, 0, 0);
        BROADCAST_LOAD_B(1, 0);
        UPDATE_RESULT_VECTOR(pg_tail, 0, 1, 0);
      }
      VECTOR_STORE(pg_tail, 0, 0);
      VECTOR_STORE(pg_tail, 0, 1);
    }
  }
  for (; j < N; j++) {

    BLASLONG i = 0;
    for (; i < v_m2; i += v_size2) {

      BLASLONG k = 0;
      DECLARE_RESULT_VECTOR(0, 0);
      DECLARE_RESULT_VECTOR(1, 0);

      for (; k < K; k++) {

        BROADCAST_LOAD_B(0, 0);
        VECTOR_LOAD_A(pg_true, 0, 0);
        UPDATE_RESULT_VECTOR(pg_true, 0, 0, 0);
        VECTOR_LOAD_A(pg_true, 1, 0);
        UPDATE_RESULT_VECTOR(pg_true, 1, 0, 0);
      }
      VECTOR_STORE(pg_true, 0, 0);
      VECTOR_STORE(pg_true, 1, 0);
    }
    for (; i < v_m1; i += v_size) {

      BLASLONG k = 0;
      DECLARE_RESULT_VECTOR(0, 0);

      for (; k < K; k++) {

        BROADCAST_LOAD_B(0, 0);
        VECTOR_LOAD_A(pg_true, 0, 0);
        UPDATE_RESULT_VECTOR(pg_true, 0, 0, 0);
      }
      VECTOR_STORE(pg_true, 0, 0);
    }
    for (; i < M; i += v_size) {
      const svbool_t pg_tail = svwhilelt_b64((uint64_t)i, (uint64_t)(M));

      BLASLONG k = 0;
      DECLARE_RESULT_VECTOR(0, 0);

      for (; k < K; k++) {

        BROADCAST_LOAD_B(0, 0);
        VECTOR_LOAD_A(pg_tail, 0, 0);
        UPDATE_RESULT_VECTOR(pg_tail, 0, 0, 0);
      }
      VECTOR_STORE(pg_tail, 0, 0);
    }
  }

  if (pack_b)
    free(packed_b);

  return 0;
}
