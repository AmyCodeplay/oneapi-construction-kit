// Copyright (C) Codeplay Software Limited. All Rights Reserved.
// CL_STD: 3.0
__kernel void store_global_int(__global int *input_buffer,
                           volatile __global atomic_int *output_buffer) {
  uint gid = get_global_id(0);
  atomic_store_explicit(output_buffer + gid, input_buffer[gid],
                        memory_order_relaxed, memory_scope_work_item);
}