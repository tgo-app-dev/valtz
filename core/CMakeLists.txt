# valtz_core -- the C++20 model-controller layer.
#
# macOS-native by design: CommonCrypto for hashing, ImageIO and
# AVFoundation for media, Metal/IOKit for hardware probing, Network /
# dns_sd for peers. Objective-C++ (.mm) is confined to the files that
# talk to Objective-C frameworks; every public header is plain C++.

set(VALTZ_CORE_SOURCES
  src/base/hash.cc
  src/base/id.cc
  src/base/log.cc
  src/base/paths.cc
  src/base/result.cc
  src/base/text.cc
  src/base/tree-copy.cc
  src/cache/cache-store.cc
  src/db/lmdb.cc
  src/media/adjust.cc
  src/media/camera.mm
  src/media/capture.mm
  src/media/color.cc
  src/media/crop.cc
  src/media/exif.mm
  src/media/format.cc
  src/media/keyframes.cc
  src/media/model-input.mm
  src/media/markup.mm
  src/media/movie.mm
  src/media/pdf.mm
  src/media/probe.mm
  src/media/sound.mm
  src/media/text-flow.cc
  src/media/thumbnail.mm
  src/media/timing.cc
  src/project/blob-store.cc
  src/project/bookmark.mm
  src/project/migrate.cc
  src/project/project.cc
  src/project/project-file.cc
  src/project/project-graph.cc
  src/project/records.cc
  src/project/undo.cc
  src/project/workspace.cc
  src/models/catalog.cc
  src/models/capabilities.cc
  src/models/hardware.mm
  src/models/model-check.cc
  src/models/drawthings.cc
  src/models/model-store.cc
  src/models/tuning.cc
  src/assist/assistant.cc
  src/assist/drama3.cc
  src/assist/moss-image.cc
  src/assist/rewrite-prompts.cc
  src/assist/transcript.cc
  src/assist/video-summary.cc
  src/agent/agent.cc
  src/agent/compaction.cc
  src/agent/cloud.mm
  src/ext/extension.cc
  src/fleet/fleet-config.cc
  src/fleet/fleet.mm
  src/engine/engine.cc
  src/engine/tensor.cc
  src/engine/null-engine.cc
  src/controller/agent.cc
  src/controller/agent-tools.cc
  src/controller/asset-ops.cc
  src/controller/atlas.cc
  src/controller/compositions.cc
  src/controller/controller.cc
  src/controller/event-bus.cc
  src/controller/job-queue.cc
  src/controller/job-timing.cc
  src/controller/locations.cc
  src/controller/log-book.cc
)

if(VALTZ_HAVE_VPIPE)
  list(APPEND VALTZ_CORE_SOURCES
    src/engine/vpipe/vpipe-engine.cc
    src/engine/vpipe/encoding-cache.cc
    src/engine/vpipe/graph-builder.cc
    src/engine/vpipe/host-exchange.cc
    src/engine/vpipe/flex-json.cc
    src/engine/vpipe/job-progress.cc
  )
endif()

# Text compiled in (resources/embed.cc.in) as valtz::resources::<symbol>:
# the model catalog, Valtz's own enhancement skills (resources/skills/),
# and the prompt-writing skills a model's makers publish under a license
# that lets Valtz ship them (3rd-party/, with their licenses). A maker's
# text that may not ship -- Qwen-Image 2.1's rewriters, non-commercial --
# is a download instead (Settings > Capabilities; Controller::skill_text).
function(valtz_embed source symbol out)
  file(READ "${source}" VALTZ_EMBED_CONTENT)
  set(VALTZ_EMBED_SOURCE "${source}")
  set(VALTZ_EMBED_SYMBOL ${symbol})
  configure_file(resources/embed.cc.in
    "${CMAKE_CURRENT_BINARY_DIR}/${out}" @ONLY)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${source}")
  set(VALTZ_CORE_SOURCES ${VALTZ_CORE_SOURCES}
    "${CMAKE_CURRENT_BINARY_DIR}/${out}" PARENT_SCOPE)
endfunction()
valtz_embed("${CMAKE_CURRENT_SOURCE_DIR}/resources/model-catalog.json"
  kModelCatalogJson model-catalog-data.cc)
# YuE2's song writer: Valtz's own, from the ComfyUI guide and the
# authors' examples (DESIGN §9).
valtz_embed("${CMAKE_CURRENT_SOURCE_DIR}/resources/skills/yue2-song.md"
  kYue2SongSkill yue2-song-skill.cc)
# MiniMax-Music3's song writer: Valtz's own, the description in the three
# sections the model was trained on and its lyrics' tags (DESIGN §4d).
valtz_embed(
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/skills/minimax-music3.md"
  kMinimaxMusic3Skill minimax-music3-skill.cc)
# The agent's guide to the editor's tools: Valtz's own (DESIGN §9b).
valtz_embed("${CMAKE_CURRENT_SOURCE_DIR}/resources/skills/valtz-agent.md"
  kValtzAgentSkill valtz-agent-skill.cc)
# MOSS-TTS's voice director: Valtz's own, from the model's prompt fields
# (TTS-Audio-Suite's MOSS_TTS_PROMPT_FIELDS_GUIDE; DESIGN §4g).
valtz_embed("${CMAKE_CURRENT_SOURCE_DIR}/resources/skills/moss-tts.md"
  kMossTtsSkill moss-tts-skill.cc)
# Drama 3's script writer: Valtz's own instructions, then Fish Audio's
# guide to directing the model as its makers publish it (CC BY 4.0,
# 3rd-party/fish-audio-drama3), read as plain text (assist/drama3.h).
valtz_embed("${CMAKE_CURRENT_SOURCE_DIR}/resources/skills/drama3.md"
  kDrama3Skill drama3-skill.cc)
valtz_embed("${PROJECT_SOURCE_DIR}/3rd-party/fish-audio-drama3/drama-3.mdx"
  kDrama3Guide drama3-guide.cc)
# Qwen-Image's prompt writer, generate and edit: Valtz's own (DESIGN §9),
# what Enhance uses unless QwenLM's rewriters have been downloaded.
valtz_embed("${CMAKE_CURRENT_SOURCE_DIR}/resources/skills/qwen-image-t2i.md"
  kQwenImageT2iSkill qwen-image-t2i-skill.cc)
valtz_embed("${CMAKE_CURRENT_SOURCE_DIR}/resources/skills/qwen-image-edit.md"
  kQwenImageEditSkill qwen-image-edit-skill.cc)
# FLUX.3 Image Klein's caption writer, generate and edit: Valtz's own,
# from vpipe's guide to the model (BFL's prompt upsampler is not used).
valtz_embed("${CMAKE_CURRENT_SOURCE_DIR}/resources/skills/flux3-klein-t2i.md"
  kFlux3KleinT2iSkill flux3-klein-t2i-skill.cc)
valtz_embed("${CMAKE_CURRENT_SOURCE_DIR}/resources/skills/flux3-klein-edit.md"
  kFlux3KleinEditSkill flux3-klein-edit-skill.cc)
# MiniMax H3's prompt-writing skill, as its makers publish it (MiniMax
# H3 Community License, 3rd-party/minimax-h3): the base guide (FL2VA)
# and the full-reference one (Ref2VA).
set(VALTZ_H3_SKILL
  "${PROJECT_SOURCE_DIR}/3rd-party/minimax-h3/skills/h3-prompt-writing")
valtz_embed("${VALTZ_H3_SKILL}/SKILL.md" kMinimaxH3Skill
  minimax-h3-skill.cc)
valtz_embed("${VALTZ_H3_SKILL}/references/base-en.txt" kMinimaxH3BaseGuide
  minimax-h3-base-guide.cc)
valtz_embed("${VALTZ_H3_SKILL}/references/ref-en.txt" kMinimaxH3RefGuide
  minimax-h3-ref-guide.cc)
# MOSS-Image's prompt-enhancer skill, as its makers publish it
# (Apache-2.0, 3rd-party/moss-image): its caption and layout drafts,
# compiled by Valtz (assist/moss-image.h) as its script compiles them.
set(VALTZ_MOSS_IMAGE_SKILL
  "${PROJECT_SOURCE_DIR}/3rd-party/moss-image/skills/moss-image-prompt-enhancer")
valtz_embed("${VALTZ_MOSS_IMAGE_SKILL}/SKILL.md" kMossImageSkill
  moss-image-skill.cc)
valtz_embed("${VALTZ_MOSS_IMAGE_SKILL}/references/layout.md"
  kMossImageLayoutGuide moss-image-layout-guide.cc)

add_library(valtz_core STATIC ${VALTZ_CORE_SOURCES}
  $<TARGET_OBJECTS:valtz_lmdb>)
add_library(valtz::core ALIAS valtz_core)

target_include_directories(valtz_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(valtz_core PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
# The system's SQLite: a Draw Things checkpoint's table of contents
# (models/drawthings.h).
target_link_libraries(valtz_core
  PUBLIC valtz_json Threads::Threads
  PRIVATE valtz_lmdb sqlite3)
target_link_libraries(valtz_core PUBLIC
  "-framework Foundation"
  "-framework CoreFoundation"
  "-framework CoreGraphics"
  "-framework CoreImage"
  "-framework CoreText"
  "-framework ColorSync"
  "-framework CoreVideo"
  "-framework CoreMedia"
  "-framework AVFoundation"
  "-framework VideoToolbox"
  "-framework AudioToolbox"
  "-framework ImageIO"
  "-framework UniformTypeIdentifiers"
  "-framework Metal"
  "-framework IOSurface"
  "-framework IOKit"
  "-framework Network"
  "-framework Security"
  "-framework ScreenCaptureKit")
# PRIVATE: definitions on a PUBLIC interface would reach swiftc, where
# -D takes no value.
target_compile_definitions(valtz_core PRIVATE
  VALTZ_VERSION="${PROJECT_VERSION}"
  $<$<BOOL:${VALTZ_HAVE_VPIPE}>:VALTZ_HAVE_VPIPE=1>)
set_source_files_properties(
  src/media/camera.mm src/media/capture.mm
  src/fleet/fleet.mm src/agent/cloud.mm
  src/media/exif.mm src/media/markup.mm src/media/model-input.mm
  src/media/movie.mm src/media/pdf.mm
  src/media/probe.mm src/media/thumbnail.mm
  src/models/hardware.mm src/project/bookmark.mm
  PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

if(VALTZ_HAVE_VPIPE)
  target_link_libraries(valtz_core PUBLIC vpipe::vpipe)
  # The engine loads valtz-vpipe.so (vpipe-plugin/) into its session. A
  # dev build also finds it in the build tree, so valtzctl and the tests
  # run in place; the app carries its own copy in Contents/PlugIns.
  target_link_libraries(valtz_core PRIVATE valtz_vpipe_contract)
  add_dependencies(valtz_core valtz-vpipe)
  set_property(SOURCE src/engine/vpipe/host-exchange.cc APPEND PROPERTY
    COMPILE_DEFINITIONS
    "VALTZ_VPIPE_PLUGIN_BUILD_PATH=\"$<TARGET_FILE:valtz-vpipe>\"")
endif()

valtz_target_warnings(valtz_core)
