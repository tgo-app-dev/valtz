#include "valtz/base/result.h"

#include <algorithm>
#include <array>

namespace valtz {

std::string
render_message(std::string_view tmpl, const MessageArgs& args)
{
  std::string out;
  out.reserve(tmpl.size());
  for (std::size_t i = 0; i < tmpl.size();) {
    if (tmpl[i] == '{') {
      const auto close = tmpl.find('}', i + 1);
      if (close != std::string_view::npos) {
        const std::string_view name = tmpl.substr(i + 1, close - i - 1);
        const auto it = std::find_if(
            args.begin(), args.end(),
            [&](const auto& a) { return a.first == name; });
        if (it != args.end()) {
          out += it->second;
          i = close + 1;
          continue;
        }
      }
    }
    out += tmpl[i++];
  }
  return out;
}

std::span<const Message>
all_messages()
{
  using namespace msg;
  static const std::array kAll = {
    kProjectNotOpen, kProjectUntitled, kProjectBusy,
    kAssetHasNoContent, kNothingToImport, kPromptEmpty,
    kUnknownModel, kModelNotInstalled, kLoraMissing, kWeightsMissing,
    kPromptRefUnbound, kPromptInUse, kNotAPrompt,
    kNoEngineForDownload, kDownloadNeedsToken, kDownloadNotGranted,
    kMarkupNeedsPicture, kMergeAcrossMask,
    kMaskNeedsLayer, kMergeOnClip, kRemoveLastLayer, kRemoveClipOwn,
    kAssetInUse, kFolderUnknown, kAssetNameEmpty, kUnfreezeKind,
    kRecursiveAsset,
    kModifyNeedsMedia,
    kPresetLoraMissing, kOutOfMemory, kUpscaleNotScaled, kUpscaleKeyed,
    kUpscaleKind, kNoUpscaler, kUpscaleFlattenFirst,
    kNotComposition, kLayerSourceKind, kSoundOnlyComposition,
    kTransitionLayers, kDecomposeNeedsComposition, kDecomposeLook,
    kDecomposeMask, kDecomposeSkew, kDecomposeRate, kDecomposeTimeline,
    kProjectCompositionStays,
    kCanvasNeedsPicture,
    kCanvasSizeOutOfRange,
    kTimelineNeedsClip, kTimelineOutOfRange,
    kPagesNeedStill, kRemoveLastPage, kNoSuchPage, kTooManyPages,
    kMergeAcrossPages, kDecomposePages,
    kNoAssistantModel, kNotAnAssistant, kModelCannotEdit, kNoEditModel,
    kEditNeedsReference,
    kNoImageModel, kNoVideoModel, kNoAudioModel, kVideoStartsFromPicture,
    kNoSpeechModel, kVoiceKind, kQuantizeNeedsSource, kNotQuantizable,
    kCaptureNeedsSound, kCaptureRunning, kNoCapture, kNoCamera, kOutputRateSet,
    kOutputColorUnknown,
    kNoReferenceModel, kVideoFirstOrReferences, kVideoReferencesNeedPicture,
    kVideoTooManyReferences, kVideoReferenceKind, kContinueNeedsClip,
    kGrabNeedsClip,
    kSourceNotBuilt, kRecipeModelUnknown, kEngineCannotRun,
    kImportFailedSome, kEnhanceUnreadable, kExportFormatUnknown,
    kExportNeedsPicture, kExportNeedsVideo, kExportNeedsSound,
    kExportNotSaved,
  };
  return kAll;
}

const char*
to_str(Code c)
{
  switch (c) {
  case Code::Ok:              return "ok";
  case Code::InvalidArgument: return "invalid argument";
  case Code::NotFound:        return "not found";
  case Code::AlreadyExists:   return "already exists";
  case Code::Io:              return "i/o error";
  case Code::Corrupt:         return "corrupt data";
  case Code::Unsupported:     return "unsupported";
  case Code::Busy:            return "busy";
  case Code::Cancelled:       return "cancelled";
  case Code::OutOfMemory:     return "out of memory";
  case Code::Engine:          return "engine error";
  case Code::Network:         return "network error";
  case Code::Internal:        return "internal error";
  }
  return "?";
}

}
