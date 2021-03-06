# Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#  * Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
#  * Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#  * Neither the name of NVIDIA CORPORATION nor the names of its
#    contributors may be used to endorse or promote products derived
#    from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
# PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
# OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

cmake_minimum_required (VERSION 3.5)

if(${TRTIS_ENABLE_HTTP_V2})
  install(
    PROGRAMS
      simple_http_v2_health_metadata.py
      simple_http_v2_infer_client.py
      simple_http_v2_string_infer_client.py
      simple_http_v2_async_infer_client.py
      simple_http_v2_sequence_sync_infer_client.py
      simple_http_v2_cudashm_client.py
      simple_http_v2_shm_client.py
      simple_http_v2_shm_string_client.py
      simple_http_v2_model_control.py
    DESTINATION python
  )
endif() # TRTIS_ENABLE_HTTP_V2

if(${TRTIS_ENABLE_GRPC_V2})
  install(
    PROGRAMS
      grpc_v2_client.py
      grpc_v2_explicit_byte_content_client.py
      grpc_v2_explicit_int_content_client.py
      grpc_v2_explicit_int8_content_client.py
      grpc_v2_image_client.py
      simple_grpc_v2_cudashm_client.py
      simple_grpc_v2_health_metadata.py
      simple_grpc_v2_async_infer_client.py
      simple_grpc_v2_infer_client.py
      simple_grpc_v2_sequence_stream_infer_client.py
      simple_grpc_v2_sequence_sync_infer_client.py
      simple_grpc_v2_string_infer_client.py
      simple_grpc_v2_shm_client.py
      simple_grpc_v2_shm_string_client.py
      simple_grpc_v2_model_control.py
    DESTINATION python
  )
endif() # TRTIS_ENABLE_GRPC_V2

if(${TRTIS_ENABLE_HTTP_V2} OR ${TRTIS_ENABLE_GRPC_V2})
  install(
    PROGRAMS
      v2_image_client.py
    DESTINATION python
  )
endif() # TRTIS_ENABLE_HTTP_V2 || TRTIS_ENABLE_GRPC_V2