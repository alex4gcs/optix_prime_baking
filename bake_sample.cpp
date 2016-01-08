
//
// Copyright (c) 2015 NVIDIA Corporation.  All rights reserved.
// 
// NVIDIA Corporation and its licensors retain all intellectual property and
// proprietary rights in and to this software, related documentation and any
// modifications thereto.  Any use, reproduction, disclosure or distribution of
// this software and related documentation without an express license agreement
// from NVIDIA Corporation is strictly prohibited.
// 
// TO THE MAXIMUM EXTENT PERMITTED BY APPLICABLE LAW, THIS SOFTWARE IS PROVIDED
// *AS IS* AND NVIDIA AND ITS SUPPLIERS DISCLAIM ALL WARRANTIES, EITHER EXPRESS
// OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.  IN NO EVENT SHALL
// NVIDIA OR ITS SUPPLIERS BE LIABLE FOR ANY SPECIAL, INCIDENTAL, INDIRECT, OR
// CONSEQUENTIAL DAMAGES WHATSOEVER (INCLUDING, WITHOUT LIMITATION, DAMAGES FOR
// LOSS OF BUSINESS PROFITS, BUSINESS INTERRUPTION, LOSS OF BUSINESS
// INFORMATION, OR ANY OTHER PECUNIARY LOSS) ARISING OUT OF THE USE OF OR
// INABILITY TO USE THIS SOFTWARE, EVEN IF NVIDIA HAS BEEN ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGES
//


#include "bake_api.h"
#include "bake_sample.h"
#include <optixu/optixu_math_namespace.h>
#include <optixu/optixu_matrix_namespace.h>
#include "random.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <vector>

using namespace optix;

// Ref: https://en.wikipedia.org/wiki/Halton_sequence
template <unsigned int BASE>
float halton(const unsigned int index)
{
  float result = 0.0f;
  const float invBase = 1.0f / BASE;
  float f = invBase;
  unsigned int i = index;
  while( i > 0 ) {
    result += f*( i % BASE );
    i = i / BASE;
    f *= invBase;
  }
  return result;
}

float3 faceforward( const float3& normal, const float3& geom_normal )
{
  if ( optix::dot( normal, geom_normal ) > 0.0f ) return normal;
  return -normal;
}

float3 operator*(const optix::Matrix4x4& mat, const float3& v)
{
  return make_float3(mat*make_float4(v, 1.0f)); 
}


void sample_triangle(const optix::Matrix4x4& xform, const optix::Matrix4x4& xform_invtrans,
                     const int3* tri_vertex_indices, const float3* verts,
                     const int3* tri_normal_indices, const float3* normals,
                     size_t tri_idx, size_t tri_sample_begin, size_t tri_sample_end,
                     float3* sample_positions, float3* sample_norms, float3* sample_face_norms, bake::SampleInfo* sample_infos,
                     size_t& sample_idx)
{
  const int3&   tri = tri_vertex_indices[tri_idx];
  const float3& v0 = verts[tri.x];
  const float3& v1 = verts[tri.y];
  const float3& v2 = verts[tri.z];

  const float3 face_normal = optix::normalize( optix::cross( v1-v0, v2-v0 ) );
  float3 n0, n1, n2;
  if (normals && tri_normal_indices) {
    const int3& nindex = tri_normal_indices[tri_idx];
    n0 = faceforward( normals[nindex.x], face_normal );
    n1 = faceforward( normals[nindex.y], face_normal );
    n2 = faceforward( normals[nindex.z], face_normal );
  } else {
    // missing vertex normals, so use face normal.
    n0 = face_normal;
    n1 = face_normal; 
    n2 = face_normal;
  }

  // Random offset per triangle, to shift Halton points
  unsigned seed = tea<4>( (unsigned)tri_idx, (unsigned)tri_idx );
  const float2 offset = make_float2( rnd(seed), rnd(seed) );

  for ( size_t index = tri_sample_begin; index < tri_sample_end; ++index, ++sample_idx )
  {
    sample_infos[sample_idx].tri_idx = (unsigned)tri_idx;
    // Note: dA must be set elsewhere
    float3& bary = *reinterpret_cast<float3*>(sample_infos[sample_idx].bary);

    // Random point in unit square
    float r1 = offset.x + halton<2>((unsigned)index+1);
    r1 = r1 - (int)r1;
    float r2 = offset.y + halton<3>((unsigned)index+1);
    r2 = r2 - (int)r2;
    assert(r1 >= 0 && r1 <= 1);
    assert(r2 >= 0 && r2 <= 1);

    // Map to triangle. Ref: PBRT 2nd edition, section 13.6.4
    const float sqrt_r1 = sqrt(r1);
    bary.x = 1.0f - sqrt_r1;
    bary.y = r2*sqrt_r1;
    bary.z = 1.0f - bary.x - bary.y;

    sample_positions[sample_idx] = xform*(bary.x*v0 + bary.y*v1 + bary.z*v2);
    sample_norms[sample_idx] = optix::normalize(xform_invtrans*( bary.x*n0 + bary.y*n1 + bary.z*n2 ));
    sample_face_norms[sample_idx] = optix::normalize(xform_invtrans*face_normal);

  }
}


double triangle_area(const float3& v0, const float3& v1, const float3& v2)
{
  float3 e0 = v1 - v0;
  float3 e1 = v2 - v0;
  float3 c = optix::cross(e0, e1);
  double x = c.x, y = c.y, z = c.z;
  return 0.5*sqrt(x*x + y*y + z*z);
}



void sample_instance(
    const bake::Instance& instance,
    const size_t min_samples_per_triangle,
    bake::AOSamples&  ao_samples
    )
{
  const bake::Mesh& mesh = *instance.mesh;
  const optix::Matrix4x4 xform(instance.xform);
  const optix::Matrix4x4 xform_invtrans = xform.inverse().transpose();
  assert( ao_samples.num_samples >= mesh.num_triangles*min_samples_per_triangle );
  assert( mesh.vertices               );
  assert( mesh.num_vertices           );
  assert( ao_samples.sample_positions );
  assert( ao_samples.sample_normals   );
  assert( ao_samples.sample_infos     );

  const int3*   tri_vertex_indices  = reinterpret_cast<int3*>( mesh.tri_vertex_indices );
  const float3* verts = reinterpret_cast<float3*>( mesh.vertices );
  const int3*   tri_normal_indices = reinterpret_cast<int3*>( mesh.tri_normal_indices );
  const float3* normals        = reinterpret_cast<float3*>( mesh.normals );

  float3* sample_positions  = reinterpret_cast<float3*>( ao_samples.sample_positions );   
  float3* sample_norms      = reinterpret_cast<float3*>( ao_samples.sample_normals   );   
  float3* sample_face_norms = reinterpret_cast<float3*>( ao_samples.sample_face_normals );
  bake::SampleInfo* sample_infos = ao_samples.sample_infos;

  size_t sample_idx = 0;  // counter for entire mesh
  std::vector<size_t> tri_sample_counts(mesh.num_triangles, 0);

  // First place minimum number of samples per triangle.
  for ( size_t tri_idx = 0; tri_idx < mesh.num_triangles; tri_idx++ )
  {
    sample_triangle(xform, xform_invtrans,
      tri_vertex_indices, verts, tri_normal_indices, normals, tri_idx, 0, min_samples_per_triangle,
      sample_positions, sample_norms, sample_face_norms, sample_infos, sample_idx /*inout*/);
    tri_sample_counts[tri_idx] += min_samples_per_triangle;
  }

  // Then do area-based sampling
  std::vector<double> tri_areas(mesh.num_triangles);
  double mesh_area = 0.0;
  for ( size_t tri_idx = 0; tri_idx < mesh.num_triangles; tri_idx++ )
  {
    const int3& tri = tri_vertex_indices[tri_idx];
    double area = triangle_area(xform*verts[tri.x], xform*verts[tri.y], xform*verts[tri.z]);
    tri_areas[tri_idx] = area;
    mesh_area += area;
  }

  const size_t num_mesh_samples = ao_samples.num_samples - sample_idx;
  for ( size_t tri_idx = 0; tri_idx < mesh.num_triangles && sample_idx < ao_samples.num_samples; tri_idx++ )
  {
    const size_t num_samples = std::min(ao_samples.num_samples - sample_idx, static_cast<size_t>(num_mesh_samples * tri_areas[tri_idx] / mesh_area));
    sample_triangle(xform, xform_invtrans,
      tri_vertex_indices, verts, tri_normal_indices, normals,
      tri_idx, tri_sample_counts[tri_idx], tri_sample_counts[tri_idx] + num_samples, 
      sample_positions, sample_norms, sample_face_norms, sample_infos, sample_idx /*inout*/);
    tri_sample_counts[tri_idx] += num_samples;
  }

  // There could be a few samples left over. Place one sample per triangle until target sample count is reached. 
  assert( ao_samples.num_samples - sample_idx <= mesh.num_triangles );
  for ( size_t tri_idx = 0; tri_idx < mesh.num_triangles && sample_idx < ao_samples.num_samples; tri_idx++) {
    sample_triangle(xform, xform_invtrans,
      tri_vertex_indices, verts, tri_normal_indices, normals, 
      tri_idx, tri_sample_counts[tri_idx], tri_sample_counts[tri_idx] + 1,
      sample_positions, 
      sample_norms, sample_face_norms, sample_infos, sample_idx /*inout*/);
    tri_sample_counts[tri_idx] += 1;
  }

  // Compute dA per sample
  {
    size_t k = 0;
    for ( size_t tri_idx = 0; tri_idx < mesh.num_triangles; ++tri_idx ) {
      assert( tri_sample_counts[tri_idx] > 0 );
      const float dA = static_cast<float>(tri_areas[tri_idx] / tri_sample_counts[tri_idx]);
      for ( size_t i = 0; i < tri_sample_counts[tri_idx]; ++i ) {
        sample_infos[k++].dA = dA;
      }
    }
  }

  assert( sample_idx == ao_samples.num_samples );

#ifdef DEBUG_MESH_SAMPLES
  for (size_t i = 0; i < ao_samples.num_samples; ++i ) {
    const SampleInfo& info = sample_infos[i];
    std::cerr << "sample info (" << i << "): " << info.tri_idx << ", (" << info.bary[0] << ", " << info.bary[1] << ", " << info.bary[2] << "), " << info.dA << std::endl;
  }
#endif

}

// entry point
size_t bake::sample_surfaces_random(
    const bake::Instance* instances,
    const size_t num_instances,
    const size_t min_samples_per_triangle,
    const size_t requested_num_samples,
    bake::AOSamples*  ao_samples
    )
{

  // First place minimum samples per instance

  size_t num_triangles = 0;
  size_t sample_count = 0;  // For entire scene
  for (size_t i = 0; i < num_instances; ++i) {
    ao_samples[i].num_samples = min_samples_per_triangle * instances[i].mesh->num_triangles; 
    sample_count += ao_samples[i].num_samples;
    num_triangles += instances[i].mesh->num_triangles;
  }
  const size_t num_samples = std::max(min_samples_per_triangle*num_triangles, requested_num_samples);

  if (num_samples > sample_count) {

    // Area-based sampling

    // Compute surface area of each instance
    double total_area = 0.0;
    std::vector<double> instance_areas(num_instances);
    std::fill(instance_areas.begin(), instance_areas.end(), 0.0);
    for (size_t idx = 0; idx < num_instances; ++idx) {
      const bake::Mesh& mesh = *instances[idx].mesh;
      const optix::Matrix4x4 xform(instances[idx].xform);
      const int3* tri_vertex_indices  = reinterpret_cast<int3*>( mesh.tri_vertex_indices );
      const float3* verts = reinterpret_cast<float3*>( mesh.vertices );
      for (size_t tri_idx = 0; tri_idx < mesh.num_triangles; ++tri_idx) {
        const int3& tri = tri_vertex_indices[tri_idx];
        double area = triangle_area(xform*verts[tri.x], xform*verts[tri.y], xform*verts[tri.z]);
        instance_areas[idx] += area;
      }
      total_area += instance_areas[idx];
    }

    const size_t num_area_based_samples = num_samples - sample_count;
    for (size_t idx = 0; idx < num_instances && sample_count < num_samples; ++idx) {
      const size_t n = std::min(num_samples - sample_count, static_cast<size_t>(num_area_based_samples * instance_areas[idx] / total_area));
      ao_samples[idx].num_samples += n;
      sample_count += n;
    }

    // There could be a few samples left over. Place one sample per instance until target sample count is reached.
    assert( num_samples - sample_count <= num_instances );
    for (size_t idx = 0; idx < num_instances && sample_count < num_samples; ++idx) {
      ao_samples[idx].num_samples += 1;
      sample_count += 1;
    }

  }

  assert(sample_count == num_samples);
  for (size_t idx = 0; idx < num_instances; ++idx) {
    const size_t n = ao_samples[idx].num_samples;
    ao_samples[idx].sample_positions    = new float[n*3];
    ao_samples[idx].sample_normals      = new float[n*3];
    ao_samples[idx].sample_face_normals = new float[n*3];
    ao_samples[idx].sample_infos        = new bake::SampleInfo[n*3];
    sample_instance(instances[idx], min_samples_per_triangle, ao_samples[idx]);

    std::cerr << "sampled instance " << idx << ": " << n << std::endl;
  }

  return num_samples;

}


