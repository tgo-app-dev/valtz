// User-facing text from the core, localizable.
//
// The core does not translate. Text a PERSON reads -- an error the app
// shows, a reason a job failed -- is a Message: a stable KEY, named
// ARGUMENTS, and the English text as the fallback. It travels with the
// Error (Error::key / args) and in bridge replies and events, and the app
// renders it in the user's language from its String Catalog
// (app/macos/Resources/Localizable.xcstrings), falling back to the English
// here for a key the catalog lacks.
//
// Why keys and not translated strings from the core: one catalog, owned
// by the app and edited with Apple's tools, holds every language; the
// core stays free of locale state and valtzctl stays English.
//
// Rules:
//   * Every key is declared once, below, with its English text, and is
//     listed in all_messages(). A unit test checks each one is in the
//     catalog with the same English text.
//   * Placeholders are `{name}`; the English text and every translation
//     use the same names. Arguments are plain strings (format numbers
//     before passing them).
//   * Logs, valtzctl output and developer diagnostics stay English and
//     are not Messages.

#ifndef VALTZ_BASE_MESSAGE_H
#define VALTZ_BASE_MESSAGE_H

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace valtz {

struct Message {
  std::string_view key;
  std::string_view english;  // with {name} placeholders
};

using MessageArgs = std::vector<std::pair<std::string, std::string>>;

// `tmpl` with each {name} replaced by its argument; an unknown {name} is
// left as written.
std::string render_message(std::string_view tmpl, const MessageArgs& args);

namespace msg {

inline constexpr Message kProjectNotOpen{
    "core.project_not_open", "The project is not open."};
inline constexpr Message kProjectUntitled{
    "core.project_untitled",
    "This project has no file yet: save it with Save As."};
inline constexpr Message kProjectBusy{
    "core.project_busy",
    "Wait for the project's work in progress to finish."};
inline constexpr Message kAssetHasNoContent{
    "core.asset_has_no_content", "This asset has no content yet."};
inline constexpr Message kNothingToImport{
    "core.nothing_to_import", "There is nothing to import."};
inline constexpr Message kPromptEmpty{
    "core.prompt_empty", "Describe what to create first."};
inline constexpr Message kUnknownModel{
    "core.unknown_model", "Unknown model: {model}."};
inline constexpr Message kPromptRefUnbound{
    "core.prompt_ref_unbound",
    "{tag} names nothing in the reference row yet: put something "
    "there first."};
inline constexpr Message kPromptInUse{
    "core.prompt_in_use",
    "“{name}” has been used to make something, so it stays as it "
    "is. Edit a copy."};
inline constexpr Message kNotAPrompt{
    "core.not_a_prompt", "“{name}” is not a prompt."};
inline constexpr Message kModelNotInstalled{
    "core.model_not_installed",
    "{model} is not installed. Download it from Models."};
inline constexpr Message kWeightsMissing{
    "core.weights_missing",
    "The checkpoint {file} is not there any more."};
inline constexpr Message kLoraMissing{
    "core.lora_missing",
    "The LoRA {file} is not there any more."};
inline constexpr Message kMarkupNeedsPicture{
    "core.markup_needs_picture",
    "Markup goes on a composition with a frame."};
inline constexpr Message kMergeAcrossMask{
    "core.merge_across_mask",
    "These layers cannot be merged: a mask ties one of them to a layer "
    "outside the two."};
inline constexpr Message kMaskNeedsLayer{
    "core.mask_needs_layer", "A mask needs a layer beneath it."};
inline constexpr Message kMergeOnClip{
    "core.merge_on_clip",
    "A clip's layers cannot be merged: they change over time."};
inline constexpr Message kRemoveLastLayer{
    "core.remove_last_layer",
    "The last layer stays: a picture has at least one."};
inline constexpr Message kRemoveClipOwn{
    "core.remove_clip_own",
    "A clip's own layer stays at the bottom: hide it instead."};
inline constexpr Message kAssetInUse{
    "core.asset_in_use",
    "{name} is still in use: another asset is made from it or shows it "
    "on a layer."};
inline constexpr Message kFolderUnknown{
    "core.folder_unknown", "There is no such folder."};
inline constexpr Message kAssetNameEmpty{
    "core.asset_name_empty", "An asset needs a name."};
inline constexpr Message kUnfreezeKind{
    "core.unfreeze_kind",
    "A picture goes onto a picture or a clip, and a clip onto a clip."};
inline constexpr Message kRecursiveAsset{
    "core.recursive_asset",
    "{name} cannot go into itself, or into anything it shows."};
inline constexpr Message kModifyNeedsMedia{
    "core.modify_needs_media",
    "Only a picture, a clip or a sound can be modified."};
inline constexpr Message kPresetLoraMissing{
    "core.preset_lora_missing",
    "{model} runs this setting with {loras}, which is not downloaded: get "
    "it in Settings › Capabilities, or choose another setting."};
inline constexpr Message kUpscaleNotScaled{
    "core.upscale_not_scaled",
    "{name} is shown at its own size or smaller: scale it up on its layer "
    "to upscale it."};
inline constexpr Message kUpscaleKeyed{
    "core.upscale_keyed",
    "{name}'s scale changes over time: an upscale renders one size, so its "
    "crop may have one keyframe at most."};
inline constexpr Message kUpscaleFlattenFirst{
    "core.upscale_flatten_first",
    "{name} is a composition: an upscaler restores one picture or clip. "
    "Make a flat copy of it first (Flatten First), then upscale that."};
inline constexpr Message kUpscaleKind{
    "core.upscale_kind", "Only a picture or a clip can be upscaled."};
inline constexpr Message kNoUpscaler{
    "core.no_upscaler",
    "No upscaler is installed for this: get one in Settings › "
    "Capabilities."};
inline constexpr Message kOutOfMemory{
    "core.out_of_memory",
    "Not enough memory: this needs about {need}, and {have} is "
    "available."};
inline constexpr Message kNotComposition{
    "core.not_composition",
    "{name} is shown as it is: only a composition has layers and looks. "
    "Change an edited copy of it."};
inline constexpr Message kLayerSourceKind{
    "core.layer_source_kind",
    "A still shows pictures, markups and frames of clips; a composition "
    "shows anything but text."};
inline constexpr Message kSoundOnlyComposition{
    "core.sound_only_composition",
    "A composition of sound holds only sounds."};
inline constexpr Message kTransitionLayers{
    "core.transition_layers",
    "A transition -- a cut or a dissolve -- goes between two layers of "
    "one composition."};
inline constexpr Message kDecomposeNeedsComposition{
    "core.decompose_needs_composition",
    "Only a layer showing a composition can be decomposed."};
inline constexpr Message kDecomposeLook{
    "core.decompose_look",
    "This layer has a look, a speed or a sound of its own: reset them to "
    "decompose it."};
inline constexpr Message kDecomposeMask{
    "core.decompose_mask",
    "A mask, or a masked layer, cannot be decomposed: release the mask "
    "first."};
inline constexpr Message kDecomposeSkew{
    "core.decompose_skew",
    "Its layers would come out skewed: a stretched layer under a turn "
    "cannot be carried over."};
inline constexpr Message kDecomposeRate{
    "core.decompose_rate",
    "Its frame rate differs from this composition's."};
inline constexpr Message kDecomposeTimeline{
    "core.decompose_timeline",
    "A timeline's layers cannot go into a still."};
inline constexpr Message kProjectCompositionStays{
    "core.project_composition_stays",
    "The project's composition stays: it is what the project is."};
inline constexpr Message kCanvasNeedsPicture{
    "core.canvas_needs_picture",
    "Only a composition with a frame has a canvas."};
inline constexpr Message kCanvasSizeOutOfRange{
    "core.canvas_size_out_of_range",
    "A canvas of {width} × {height} pixels cannot be made."};
inline constexpr Message kTimelineNeedsClip{
    "core.timeline_needs_clip", "Only a composition has a timeline."};
inline constexpr Message kPagesNeedStill{
    "core.pages_need_still", "Only a still composition has pages."};
inline constexpr Message kRemoveLastPage{
    "core.remove_last_page",
    "The last page stays: a still has at least one."};
inline constexpr Message kNoSuchPage{
    "core.no_such_page", "There is no page {page}."};
inline constexpr Message kTooManyPages{
    "core.too_many_pages", "A still holds at most {pages} pages."};
inline constexpr Message kMergeAcrossPages{
    "core.merge_across_pages",
    "These layers cannot be merged: they are on different pages, or "
    "change from page to page."};
inline constexpr Message kDecomposePages{
    "core.decompose_pages",
    "A still with pages cannot be decomposed: its layers are on pages of "
    "their own."};
inline constexpr Message kTimelineOutOfRange{
    "core.timeline_out_of_range", "That timeline is too long."};
inline constexpr Message kNoEngineForDownload{
    "core.no_engine_for_download",
    "Models cannot be downloaded without the engine."};
inline constexpr Message kDownloadNeedsToken{
    "core.download_needs_token",
    "{model} is a gated model: Hugging Face needs your access token to "
    "download it. Accept its license on its page, then download it with a "
    "token that can read."};
inline constexpr Message kDownloadNotGranted{
    "core.download_not_granted",
    "Hugging Face refused the token for {model}: accept its license on its "
    "page with the same account, and check that the token can read."};
inline constexpr Message kNoAssistantModel{
    "core.no_assistant_model", "No assistant model fits this Mac."};
inline constexpr Message kNotAnAssistant{
    "core.not_an_assistant", "{model} is not an agentic helper."};
inline constexpr Message kModelCannotEdit{
    "core.model_cannot_edit", "{model} cannot edit images."};
inline constexpr Message kNoEditModel{
    "core.no_edit_model", "No image-editing model is installed."};
inline constexpr Message kEditNeedsReference{
    "core.edit_needs_reference", "An edit needs a reference picture."};
inline constexpr Message kNoImageModel{
    "core.no_image_model", "No image model is selected."};
inline constexpr Message kNoVideoModel{
    "core.no_video_model", "No video model is installed."};
inline constexpr Message kNoAudioModel{
    "core.no_audio_model", "No audio model is installed."};
inline constexpr Message kNoSpeechModel{
    "core.no_speech_model",
    "No speech model is ready: MOSS-TTS and its audio tokenizer are "
    "needed."};
inline constexpr Message kVoiceKind{
    "core.voice_kind",
    "{name} cannot be a voice: speech clones a sound, or a clip's sound."};
inline constexpr Message kQuantizeNeedsSource{
    "core.quantize_needs_source",
    "{source} is needed first: {model} is quantized from it."};
inline constexpr Message kCaptureNeedsSound{
    "core.capture_needs_sound",
    "Sound is recorded into a composition of sound alone."};
inline constexpr Message kCaptureRunning{
    "core.capture_running", "A recording is running already."};
inline constexpr Message kNoCapture{
    "core.no_capture", "No recording is running."};
inline constexpr Message kOutputRateSet{
    "core.output_rate_set",
    "The frame rate is the project timeline's: it changes only while the "
    "timeline is empty."};
inline constexpr Message kOutputColorUnknown{
    "core.output_color_unknown", "{color} is not an output colour space."};
inline constexpr Message kNoCamera{
    "core.no_camera", "The camera is not on, or not recording."};
inline constexpr Message kNotQuantizable{
    "core.not_quantizable", "{model} is not made by quantizing."};
inline constexpr Message kNoReferenceModel{
    "core.no_reference_model",
    "No video model that reads references is installed."};
inline constexpr Message kVideoFirstOrReferences{
    "core.video_first_or_references",
    "A clip opens on a picture or reads references, not both."};
inline constexpr Message kVideoReferencesNeedPicture{
    "core.video_references_need_picture",
    "A clip's references need a picture or a clip besides sounds."};
inline constexpr Message kVideoTooManyReferences{
    "core.video_too_many_references",
    "A clip reads at most {pictures} pictures, {clips} clips and "
    "{sounds} sounds, {total} in all."};
inline constexpr Message kVideoReferenceKind{
    "core.video_reference_kind",
    "{name} cannot be a reference: a clip reads pictures, clips and "
    "sounds."};
inline constexpr Message kContinueNeedsClip{
    "core.continue_needs_clip",
    "Only a clip can be continued."};
inline constexpr Message kGrabNeedsClip{
    "core.grab_needs_clip",
    "A frame can only be grabbed from a clip."};
inline constexpr Message kVideoStartsFromPicture{
    "core.video_starts_from_picture",
    "A clip can start from a picture only."};
inline constexpr Message kSourceNotBuilt{
    "core.source_not_built", "Imported assets are not built."};
inline constexpr Message kRecipeModelUnknown{
    "core.recipe_model_unknown",
    "This asset was made with {model}, which this version of Valtz does "
    "not know."};
inline constexpr Message kEngineCannotRun{
    "core.engine_cannot_run", "The engine cannot run {operation}."};
inline constexpr Message kImportFailedSome{
    "core.import_failed_some",
    "{failed} of {total} files could not be imported."};
inline constexpr Message kEnhanceUnreadable{
    "core.enhance_unreadable",
    "The assistant's answer could not be read. Try again."};
inline constexpr Message kExportFormatUnknown{
    "core.export_format_unknown", "Valtz cannot export to {format}."};
inline constexpr Message kExportNeedsPicture{
    "core.export_needs_picture", "{format} is a format for pictures."};
inline constexpr Message kExportNeedsVideo{
    "core.export_needs_video", "{format} is a format for videos."};
inline constexpr Message kExportNeedsSound{
    "core.export_needs_sound", "{format} is a format for sounds."};
inline constexpr Message kExportNotSaved{
    "core.export_not_saved", "The export could not be saved to {path}."};

}  // namespace msg

// Every Message above, for the catalog check.
std::span<const Message> all_messages();

}

#endif
