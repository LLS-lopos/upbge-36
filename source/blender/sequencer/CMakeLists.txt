# SPDX-FileCopyrightText: 2011-2024 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  .
  intern
  ../animrig
  ../imbuf/movie
  ../makesrna

  # RNA_prototypes.hh
  ${CMAKE_BINARY_DIR}/source/blender/makesrna
)

set(INC_SYS

)

set(SRC
  SEQ_add.hh
  SEQ_animation.hh
  SEQ_channels.hh
  SEQ_connect.hh
  SEQ_edit.hh
  SEQ_effects.hh
  SEQ_iterator.hh
  SEQ_modifier.hh
  SEQ_prefetch.hh
  SEQ_proxy.hh
  SEQ_relations.hh
  SEQ_render.hh
  SEQ_retiming.hh
  SEQ_select.hh
  SEQ_sequencer.hh
  SEQ_sound.hh
  SEQ_thumbnail_cache.hh
  SEQ_time.hh
  SEQ_transform.hh
  SEQ_utils.hh

  intern/animation.cc
  intern/channels.cc
  intern/disk_cache.cc
  intern/disk_cache.hh
  intern/effects.cc
  intern/effects.hh
  intern/image_cache.cc
  intern/image_cache.hh
  intern/iterator.cc
  intern/media_presence.cc
  intern/modifier.cc
  intern/multiview.cc
  intern/multiview.hh
  intern/prefetch.cc
  intern/prefetch.hh
  intern/proxy.cc
  intern/proxy.hh
  intern/proxy_job.cc
  intern/render.cc
  intern/render.hh
  intern/sequence_lookup.cc
  intern/sequencer.cc
  intern/sequencer.hh
  intern/sound.cc
  intern/strip_add.cc
  intern/strip_connect.cc
  intern/strip_edit.cc
  intern/strip_relations.cc
  intern/strip_retiming.cc
  intern/strip_select.cc
  intern/strip_time.cc
  intern/strip_time.hh
  intern/strip_transform.cc
  intern/thumbnail_cache.cc
  intern/utils.cc
  intern/utils.hh
)

set(LIB
  PRIVATE bf::blenfont
  PRIVATE bf::blenkernel
  PRIVATE bf::blenlib
  PRIVATE bf::blenloader
  PRIVATE bf::blentranslation
  PRIVATE bf::depsgraph
  PRIVATE bf::dna
  PRIVATE bf::imbuf
  PRIVATE bf::intern::atomic
  PRIVATE bf::intern::clog
  PRIVATE bf::intern::guardedalloc
  PRIVATE bf::render
  PRIVATE bf::windowmanager
)

if(WITH_AUDASPACE)
  list(APPEND INC_SYS
    ${AUDASPACE_C_INCLUDE_DIRS}
  )
  if(WITH_SYSTEM_AUDASPACE)
    list(APPEND LIB
      ${AUDASPACE_C_LIBRARIES}
      ${AUDASPACE_PY_LIBRARIES}
    )
  endif()
  add_definitions(-DWITH_AUDASPACE)

  if(WITH_FFTW3)
    add_definitions(-DWITH_CONVOLUTION)
  endif()
endif()

blender_add_lib(bf_sequencer "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")

# RNA_prototypes.hh
add_dependencies(bf_sequencer bf_rna)
