// The controller's AGENT: its sessions, the loop each runs, the tools it
// calls (agent-tools.cc), the backends its turns go to -- this Mac's
// helper or a fleet member's (here), a cloud API (agent/cloud.mm) -- and
// the turns this Mac serves for a member's sessions. DESIGN §9b.
//
// A private header: agent.cc and agent-tools.cc only.

#ifndef VALTZ_CONTROLLER_AGENT_HOST_H
#define VALTZ_CONTROLLER_AGENT_HOST_H

#include "valtz/agent/agent.h"
#include "valtz/assist/assistant.h"
#include "valtz/controller/controller.h"
#include "valtz/fleet/fleet.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace valtz {

class Controller::AgentHost {
public:
  explicit AgentHost(Controller& c);
  ~AgentHost();

  Result<std::string> start(AgentRequest);
  Status say(const std::string& id, const std::string& text,
             const Json& stage = Json::object(),
             const std::vector<AssetId>& attached = {});
  Status review(const std::string& id, int step, const std::string& what,
                const Json& args, const std::string& note);
  Status set_review(const std::string& id, bool review);
  Status pause(const std::string& id);
  Status resume(const std::string& id);
  Status stop(const std::string& id);
  Json state(const std::string& id) const;
  Json list(ProjectId) const;

  Json settings() const;
  Status set_settings(const Json&);
  // Its settings' "model" ("" the Agentic Helper's), "memory_gb" (0
  // none), "expert_cache_gb" (0 Auto), "i8_prefill" ("auto" the
  // model's own, "on", "off") and "ngram_resident" (false): Controller::
  // agent_model, agent_memory_limit_, agent_expert_cache_,
  // agent_i8_prefill_, agent_ngram_asked_.
  std::string model_choice() const;
  int memory_gb() const;
  int expert_cache_gb() const;
  std::string i8_prefill() const;
  bool ngram_resident() const;
  Json tools_json() const;

  // A job's event as the controller posts it: an agent's picture landing
  // (handled on the host's own thread, never the poster's) -- or the
  // prompt enhancer's reply (assist.enhanced), kept for the tool that
  // asked for it.
  void on_event(const std::string& kind, JobId job, const Json& data);
  // The project closing: its sessions stopped.
  void project_closing(ProjectId);

  // ---- turns served for a fleet member's sessions
  // The engine job for a member's turn: the helper's, its context kept
  // under the member's key in this Mac's fleet folder, as long as its
  // lease. `dir`: where a served job's files go.
  Result<engine::JobSpec> served_turn(const fleet::Offer&,
                                      const std::string& from,
                                      const std::filesystem::path& dir);
  // A member's session over: its context let go.
  void close_served(const std::string& from, const std::string& key);

  void shutdown();

  // ---- a session
  struct Step {
    int                    n = 0;
    std::string            call_id;
    std::string            tool;
    std::string            title;
    Json                   args = Json::object();
    // proposed | running | waiting (its picture not yet landed) | done
    // | failed | skipped
    std::string            state;
    Json                   result;
    std::string            error;
    std::optional<JobId>   job;
  };
  // Where a picture it generates goes when it lands -- or a SOUND (a
  // song, a voice-over): onto a timeline from `start`, cut at `seconds`,
  // at its volume, faded in and out -- or a SHOT (generate_video): onto a
  // timeline from `start`, over the clips before it, filling the frame.
  struct Landing {
    std::string                 session;
    int                         step = 0;
    ProjectId                   project;
    std::optional<AssetId>      composition;
    std::int64_t                page = 0;      // from 0
    std::optional<std::array<double, 4>> box;  // x, y, width, height
    bool                        bottom = false;
    bool                        sound = false;
    std::int64_t                start = 0;     // the timeline's frames
    double                      seconds = 0;   // 0: as long as it is
    double                      volume = 1;
    double                      fade_in = 0;
    double                      fade_out = 0;
    // Music: ended with the clips, if they end before it would.
    bool                        fit = false;
    bool                        clip = false;  // a shot
    // The model it runs on, by name: told with what landed -- or failed
    // -- so the agent can say which made it.
    std::string                 model;
  };
  struct Decision {
    int         step = 0;
    std::string what;
    Json        args;
    std::string note;
  };
  struct Session {
    std::string                         id;
    ProjectId                           project;
    std::optional<AssetId>              target;
    std::string                         prompt;
    std::vector<AssetId>                row;
    // Its Valtz tools (AgentRequest::skills): "full" alone edits and
    // holds the project.
    std::string                         skills = "full";
    std::vector<agent::Message>         messages;
    std::vector<Step>                   steps;
    std::unique_ptr<agent::Backend>     backend;
    std::filesystem::path               dir;
    mutable std::mutex                  mu;
    std::condition_variable             cv;
    std::string                         state = "starting";
    bool                                review = true;
    bool                                pause_asked = false;
    bool                                stop_asked = false;
    std::atomic<bool>                   cancel{false};
    // The person's words said while it worked or listened, as the user
    // messages they become (person_message_).
    std::deque<agent::Message>          said;
    std::optional<Decision>             decision;
    // Its pictures not yet landed, and what landed since the model was
    // last told.
    std::set<JobId>                     pending;
    std::vector<Json>                   landed;
    // wait_for_generations calls that found nothing to wait for.
    int                                 idle_waits = 0;
    // Its SHOTS on each timeline (generate_video): the step of the last
    // one asked for -- what the next `continues` from -- and where the
    // shots asked for end, timeline seconds: where the next one starts.
    struct Shots {
      int         last = 0;
      double      end = 0;
      int         count = 0;
      // The subjects last given on it: a continuation that gives none
      // carries them on.
      std::string subjects;
    };
    std::map<AssetId, Shots>            shots;
    // Shots HELD until the one they carry on from lands (their steps).
    std::set<int>                       held;
    // The person's question it asked (ask_user), and the answer.
    std::optional<std::string>          answer;
    std::string                         writing;
    std::string                         error;
    // Its message as the app words it: {message, key, args}.
    Json                                error_fields = Json::object();
    int                                 turns = 0;
    // At work: holding its project (AgentHost::post_). The project's last
    // command as it stopped working (done, failed): the person's own
    // changes since are told with the next words said to it -- what it
    // last saw is out of date.
    bool                                working = false;
    std::uint64_t                       seen_seq = 0;
    Json                                usage = Json::object();
    // The last turn's cost and speed (agent::turn_stats).
    Json                                stats = Json::object();
    // COMPACTION (agent::compact): the tokens the last turn's
    // conversation took, as its backend counted them, and the messages
    // it held then; the messages compacted away -- shown and saved,
    // never sent again -- and how many times it was.
    std::int64_t                        ctx_used = 0;
    std::size_t                         ctx_counted = 0;
    std::vector<agent::Message>         earlier;
    int                                 compactions = 0;
    // Settings › Agentic Helper's Compact Long Conversations, as it
    // started: off, a session ends where its model's context does.
    bool                                compact = true;
    std::int64_t                        created_ms = 0;
    // Its TRANSCRIPT in the project's Assets (write_transcript_): made
    // as it first rests, written again at each rest after.
    std::optional<AssetId>              transcript;
    std::thread                         thread;
  };

  // ---- a tool (agent-tools.cc)
  using ToolFn = Result<Json> (AgentHost::*)(
      Session&, Step&, const Json&, std::vector<std::filesystem::path>&);
  struct Tool {
    std::string name;
    std::string kind;   // read | edit | generate
    std::string title;
    std::string description;
    Json        parameters;
    ToolFn      run = nullptr;
  };
  static const std::vector<Tool>& tools();
  // A CHAT's tools ("chat"): the full table's that look -- at the
  // project, its pictures, clips and sounds -- and that ADD to its assets
  // (a picture made as an asset alone, a text kept, a prompt saved);
  // none that changes what is there.
  static const std::vector<Tool>& chat_tools();
  // The tools of a session's skills: "full", "chat", "none" (none).
  static const std::vector<Tool>& tools_for(std::string_view skills);
  static std::vector<agent::ToolSpec>
  tool_specs(std::string_view skills = "full");

private:
  // This Mac's helper, or a fleet member's (agent.cc).
  class HelperBackend;

  void run_(Session& s);
  // Would the next turn outgrow what its model holds, with room for its
  // reply and for the notes?
  bool needs_compaction_(Session& s,
                         const std::vector<agent::ToolSpec>& specs) const;
  // The conversation compacted (agent::compact): the model asked for its
  // notes -- unless `full`, the last turn not fitting -- and the latest
  // turns kept. False when stopped meanwhile.
  bool compact_(Session& s, const std::vector<agent::ToolSpec>& specs,
                bool full);
  // Between steps: paused, it waits; stopped, false.
  bool gate_(Session& s);
  // The person's next words (done, failed): false when stopped.
  bool wait_words_(Session& s);
  // The person's words said meanwhile into the conversation, in order,
  // their pictures shown (show_attached_).
  void take_said_(Session& s);
  void set_state_(Session& s, std::string state);
  // agent.changed posted -- and the project held while it works.
  void post_(Session& s, std::optional<int> step = std::nullopt);
  // The project's last command done (0: none).
  std::uint64_t last_command_(ProjectId) const;
  Json state_json_(const Session& s, bool messages) const;
  void save_(Session& s);
  std::shared_ptr<Session> find_(const std::string& id) const;
  // The system message: the guide, the project as it is, the request.
  std::string system_text_(const Session& s);
  // Where the person is as they say `words` (AgentRequest::stage), told
  // after them: the composition, its page or frame, the selected layer
  // and objects; the words alone with nothing to say.
  std::string with_stage_(ProjectId pid, const std::string& words,
                          const Json& stage);
  // The person's words as the user message the model is handed: where
  // they were, and -- `list` -- the assets they attached, by name and
  // id for the tools. Their own words and those ids kept apart (said,
  // attached) for the app and the transcript.
  // `tools`: told by id, for them; without tools, by name alone.
  agent::Message person_message_(ProjectId pid, const std::string& words,
                                 const Json& stage,
                                 const std::vector<AssetId>& attached,
                                 bool list, bool tools = true);
  // The pictures among a person's message's attachments SHOWN with it,
  // where its model sees: drawn small into the session's folder, the
  // first kShownAttached of them. On the session's thread, before the
  // message joins the conversation.
  void show_attached_(Session& s, agent::Message& m);
  // The tokens the conversation takes now, as its next turn would be
  // handed it: the last turn's count and an estimate of what came since
  // (all estimated before any) -- what the app's ring shows.
  std::int64_t context_estimate_(const Session& s) const;
  // The session's TRANSCRIPT (DESIGN §9b): its conversation in Markdown
  // -- the person's words and attachments, the agent's words, each step
  // with what it was asked and how it went, Valtz's notes, the
  // compactions -- a text asset of the project, in its Agent Sessions
  // folder, written at each rest (done, failed, stopped). In no command:
  // a record of what happened, not undone with the steps it tells.
  void write_transcript_(Session& s);
  std::string transcript_text_(const Session& s) const;
  std::unique_ptr<agent::Backend> backend_(const std::string& kind,
                                           const std::string& key,
                                           const std::filesystem::path& dir);
  // Its pictures waited for (wait_for_generations; done with some
  // still out): false when stopped.
  bool wait_pending_(Session& s);
  // What landed since the model was last told, as a user message.
  void tell_landed_(Session& s);
  void land_(JobId job, const std::string& kind, const Json& data);
  void housekeeping_();

  // ---- the tools (agent-tools.cc)
  Result<Json> t_get_project(Session&, Step&, const Json&,
                             std::vector<std::filesystem::path>&);
  Result<Json> t_find_objects(Session&, Step&, const Json&,
                              std::vector<std::filesystem::path>&);
  Result<Json> t_get_composition(Session&, Step&, const Json&,
                                 std::vector<std::filesystem::path>&);
  Result<Json> t_view_page(Session&, Step&, const Json&,
                           std::vector<std::filesystem::path>&);
  Result<Json> t_get_exif(Session&, Step&, const Json&,
                          std::vector<std::filesystem::path>&);
  Result<Json> t_view_asset(Session&, Step&, const Json&,
                            std::vector<std::filesystem::path>&);
  Result<Json> t_create_composition(Session&, Step&, const Json&,
                                    std::vector<std::filesystem::path>&);
  Result<Json> t_add_pages(Session&, Step&, const Json&,
                           std::vector<std::filesystem::path>&);
  Result<Json> t_remove_page(Session&, Step&, const Json&,
                             std::vector<std::filesystem::path>&);
  Result<Json> t_add_text(Session&, Step&, const Json&,
                          std::vector<std::filesystem::path>&);
  Result<Json> t_add_shape(Session&, Step&, const Json&,
                           std::vector<std::filesystem::path>&);
  Result<Json> t_update_object(Session&, Step&, const Json&,
                               std::vector<std::filesystem::path>&);
  Result<Json> t_move_objects(Session&, Step&, const Json&,
                              std::vector<std::filesystem::path>&);
  Result<Json> t_remove_object(Session&, Step&, const Json&,
                               std::vector<std::filesystem::path>&);
  Result<Json> t_arrange_objects(Session&, Step&, const Json&,
                                 std::vector<std::filesystem::path>&);
  Result<Json> t_draw_strokes(Session&, Step&, const Json&,
                              std::vector<std::filesystem::path>&);
  // ---- a long text on pages (agent-tools.cc)
  Result<Json> t_flow_text(Session&, Step&, const Json&,
                           std::vector<std::filesystem::path>&);
  Result<Json> t_add_page_progress(Session&, Step&, const Json&,
                                   std::vector<std::filesystem::path>&);
  Result<Json> t_place_asset(Session&, Step&, const Json&,
                             std::vector<std::filesystem::path>&);
  Result<Json> t_update_composition(Session&, Step&, const Json&,
                                    std::vector<std::filesystem::path>&);
  Result<Json> t_set_layer_box(Session&, Step&, const Json&,
                               std::vector<std::filesystem::path>&);
  Result<Json> t_upscale_layer(Session&, Step&, const Json&,
                               std::vector<std::filesystem::path>&);
  Result<Json> t_set_layer_pages(Session&, Step&, const Json&,
                                 std::vector<std::filesystem::path>&);
  Result<Json> t_arrange_layer(Session&, Step&, const Json&,
                               std::vector<std::filesystem::path>&);
  Result<Json> t_remove_layer(Session&, Step&, const Json&,
                              std::vector<std::filesystem::path>&);
  Result<Json> t_rename_layer(Session&, Step&, const Json&,
                              std::vector<std::filesystem::path>&);
  Result<Json> t_set_layer_mask(Session&, Step&, const Json&,
                                std::vector<std::filesystem::path>&);
  Result<Json> t_add_mask(Session&, Step&, const Json&,
                          std::vector<std::filesystem::path>&);
  Result<Json> t_set_layer_visible(Session&, Step&, const Json&,
                                   std::vector<std::filesystem::path>&);
  Result<Json> t_adjust_layer(Session&, Step&, const Json&,
                              std::vector<std::filesystem::path>&);
  Result<Json> t_key_layer(Session&, Step&, const Json&,
                           std::vector<std::filesystem::path>&);
  Result<Json> t_group_layers(Session&, Step&, const Json&,
                              std::vector<std::filesystem::path>&);
  Result<Json> t_ungroup_layers(Session&, Step&, const Json&,
                                std::vector<std::filesystem::path>&);
  Result<Json> t_duplicate_layer(Session&, Step&, const Json&,
                                 std::vector<std::filesystem::path>&);
  Result<Json> t_merge_layers(Session&, Step&, const Json&,
                              std::vector<std::filesystem::path>&);
  Result<Json> t_set_layer_source(Session&, Step&, const Json&,
                                  std::vector<std::filesystem::path>&);
  Result<Json> t_lock_layer(Session&, Step&, const Json&,
                            std::vector<std::filesystem::path>&);
  Result<Json> t_flatten_layer(Session&, Step&, const Json&,
                               std::vector<std::filesystem::path>&);
  Result<Json> t_decompose_layer(Session&, Step&, const Json&,
                                 std::vector<std::filesystem::path>&);
  Result<Json> t_rename_asset(Session&, Step&, const Json&,
                              std::vector<std::filesystem::path>&);
  Result<Json> t_file_assets(Session&, Step&, const Json&,
                             std::vector<std::filesystem::path>&);
  Result<Json> t_remove_asset(Session&, Step&, const Json&,
                              std::vector<std::filesystem::path>&);
  Result<Json> t_grab_frame(Session&, Step&, const Json&,
                            std::vector<std::filesystem::path>&);
  Result<Json> t_get_recipe(Session&, Step&, const Json&,
                            std::vector<std::filesystem::path>&);
  Result<Json> t_make_refmod(Session&, Step&, const Json&,
                              std::vector<std::filesystem::path>&);
  Result<Json> t_create_prompt(Session&, Step&, const Json&,
                               std::vector<std::filesystem::path>&);
  Result<Json> t_generate_image(Session&, Step&, const Json&,
                                std::vector<std::filesystem::path>&);
  // ---- a chat's own (chat_tools): a picture made as an asset alone --
  // generate_image placed nowhere, whatever it is given -- and words
  // kept as a text asset.
  Result<Json> t_chat_picture(Session&, Step&, const Json&,
                              std::vector<std::filesystem::path>&);
  Result<Json> t_save_text(Session&, Step&, const Json&,
                           std::vector<std::filesystem::path>&);
  // The MODELS of a task -- pictures, shots, music, ... -- that run here,
  // Auto's pick among them, and those not downloaded: what the person
  // may choose from by name (each tool's `model`).
  Result<Json> t_list_models(Session&, Step&, const Json&,
                             std::vector<std::filesystem::path>&);
  // A model's FAVOR and CUSTOM OPTIONS (Pika H3 Flex, HyperFlow,
  // TaoMate, Sage, steps, its few-step LoRAs ...): what each generating
  // tool's `favor` and `options` may ask of it, and what is not here.
  Result<Json> t_model_options(Session&, Step&, const Json&,
                               std::vector<std::filesystem::path>&);
  // The PROMPT ENHANCER (Controller::enhance_prompt) on the agent's words:
  // the fields of the tool that makes it, and what to look at in them.
  Result<Json> t_enhance_prompt(Session&, Step&, const Json&,
                                std::vector<std::filesystem::path>&);
  Result<Json> t_wait_for_generations(Session&, Step&, const Json&,
                                      std::vector<std::filesystem::path>&);
  Result<Json> t_ask_user(Session&, Step&, const Json&,
                          std::vector<std::filesystem::path>&);
  // ---- timelines: clips, sounds, music (agent-tools.cc)
  Result<Json> t_describe_clip(Session&, Step&, const Json&,
                               std::vector<std::filesystem::path>&);
  // ASSET OPERATIONS (DESIGN §4j): what was said in a sound or a clip; a
  // song split into its voice and the rest.
  Result<Json> t_transcribe(Session&, Step&, const Json&,
                            std::vector<std::filesystem::path>&);
  Result<Json> t_separate_sound(Session&, Step&, const Json&,
                                std::vector<std::filesystem::path>&);
  Result<Json> t_view_frames(Session&, Step&, const Json&,
                             std::vector<std::filesystem::path>&);
  Result<Json> t_add_clip(Session&, Step&, const Json&,
                          std::vector<std::filesystem::path>&);
  Result<Json> t_add_sound(Session&, Step&, const Json&,
                           std::vector<std::filesystem::path>&);
  Result<Json> t_set_layer_time(Session&, Step&, const Json&,
                                std::vector<std::filesystem::path>&);
  Result<Json> t_set_layer_sound(Session&, Step&, const Json&,
                                 std::vector<std::filesystem::path>&);
  Result<Json> t_set_transition(Session&, Step&, const Json&,
                                std::vector<std::filesystem::path>&);
  Result<Json> t_set_timeline_length(Session&, Step&, const Json&,
                                     std::vector<std::filesystem::path>&);
  Result<Json> t_generate_music(Session&, Step&, const Json&,
                                std::vector<std::filesystem::path>&);
  Result<Json> t_generate_voiceover(Session&, Step&, const Json&,
                                    std::vector<std::filesystem::path>&);
  // ---- scenes: shots of a clip with its sound (agent-tools.cc)
  Result<Json> t_generate_video(Session&, Step&, const Json&,
                                std::vector<std::filesystem::path>&);

  // Tool helpers (agent-tools.cc)
  //
  // The MODEL a task runs on as the person names it (each tool's `model`):
  // its id or its name as shown, loosely -- "FLUX.3", "music3" -- or null
  // for Auto's ("", "auto"), which each tool works out as before. One
  // that does not do the task, or is not on this Mac, is refused naming
  // those here that do: the agent tells the person, or leaves it out.
  Result<const models::ModelEntry*> model_for_(std::string_view task,
                                               const std::string& named)
      const;
  // A model of the task here: "here" (runs now), "downloading" (its job
  // waits for it), "" (not here).
  std::string model_state_(const models::ModelEntry&,
                           std::string_view task) const;
  // Auto's model for the task -- for upscales, a clip's and a picture's.
  std::vector<const models::ModelEntry*> auto_for_(std::string_view task)
      const;
  // The speech model a voice-over speaks with by default: the best-ranked
  // that runs here.
  const models::ModelEntry* script_model_() const;
  const models::ModelEntry* voice_model_() const;
  // FAVOR AND ITS OPTIONS as the agent names them (each generating tool's
  // `favor` and `options`, DESIGN §9b): the request's preference and
  // Custom values for `m`, each name read loosely against what `m`
  // offers here -- "the Pika LoRA" is the Pika H3 Flex switch, which
  // runs its adapter WITH its own schedule; "the 4-step LoRA" a few-step
  // LoRA of the model's -- and what each was read as, for the agent to
  // say. Refused, naming what it offers or why: a name it has nothing
  // for, two that do not go together, an adapter not downloaded.
  struct Tuned {
    std::string preference = "balanced";
    Json tuning = Json::object();
    bool given = false;           // a favor or options asked for
    Json read = Json::array();    // [{"asked", "as", "value"}]
    std::vector<std::string> notes;
    Json runs;                    // what it settles to, told back
  };
  Result<Tuned> tuned_(const models::ModelEntry& m, const Json& args,
                       bool edit, bool continuation, bool opens_on_picture)
      const;
  // How the clip a shot comes after was made -- the timeline's last, the
  // session's shot before it on that timeline (made, being made or
  // held), the clip it continues from: its Favor, where its request was
  // recorded, and its METHOD's switches on (Pika H3 Flex, HyperFlow,
  // TaoMate, VDN). A shot after it is made the same unless it says its
  // own (t_generate_video): a style kept shot to shot. Nothing for an
  // imported clip, or none.
  struct MadeAs {
    std::string clip;                     // its name
    std::string favor;                    // Fast | Med | Fine; "": unknown
    Json        options = Json::object(); // switch key: true
  };
  std::optional<MadeAs> made_before_(Session& s,
                                     std::optional<AssetId> comp,
                                     std::optional<AssetId> from);
  // The settled values said shortly: the favor, the steps, the adapters
  // and switches on.
  Json runs_with_(const Json& values, std::string_view preference,
                  const Json& lora_names) const;
  // What `t` was read as, told in a tool's result: "runs_with", and the
  // names as read ("options_read") and what came of them.
  static void told_(Json& result, const Tuned& t);
  Result<AssetId> composition_(const Session& s, const Json& args);
  Result<std::string> layer_(const Json& args);
  // A page argument (from 1) as the composition's (from 0), checked.
  // `named`: on a still of more than one page it must be given.
  Result<std::int64_t> page_(ProjectId, AssetId comp, const Json& args,
                             const char* key = "page", bool named = false);
  // A layer's box on its composition's frame, from its placement.
  Json layer_box_(ProjectId, const project::Asset& comp,
                  const project::Layer&);
  // The crop that lays content of `content` pixels in a box of the frame.
  static media::Crop box_crop_(media::PixelSize content,
                               media::PixelSize frame,
                               const std::array<double, 4>& box);
  // `to` -- a layer's new box -- with the turn, flips and padding `was`
  // had: the person's, kept when the agent moves the layer.
  static media::Crop kept_turn_(const media::Crop& was, media::Crop to);
  Result<std::array<double, 4>> box_(const Json& args,
                                     media::PixelSize frame,
                                     bool line = false);
  // The markup layer a page's words and shapes go on, and its objects:
  // the page's OWN -- on that page alone; one running across pages is
  // passed over, or words meant for one page would show on all.
  Result<std::string> markup_on_(ProjectId, AssetId comp,
                                 std::int64_t page);
  // A layer made for one page of a still put on that page ALONE: on a
  // still of one page a layer runs on to the last page, and pages added
  // later (add_pages, flow_text) would repeat it.
  Status page_alone_(ProjectId, AssetId comp, const std::string& layer,
                     std::int64_t page);
  // The markups across EVERY page of a still: the FRONT one on top (a
  // header, a footer, a frame), the BACK one under every layer (a
  // background) -- each made, and named, when there is none. With `page`,
  // BACK is that page's own back markup, right above the one across
  // every page.
  Result<std::string> front_markup_(ProjectId, AssetId comp);
  Result<std::string> back_markup_(ProjectId, AssetId comp,
                                   std::optional<std::int64_t> page);
  // A layer laid under a page's others: right above the back markup
  // across every page, where there is one (else at the bottom).
  Status to_back_(ProjectId, AssetId comp, const std::string& layer);
  // The words already on a still's page that `box` would lie over --
  // the page's own text boxes, as far as their words reach (`skip`: the
  // object just made) -- told to the model with a note, or nothing.
  Json crowding_(ProjectId, AssetId comp, std::int64_t page,
                 const std::array<double, 4>& box,
                 const std::string& skip = {});
  // flow_text's words: the passage of the person's own messages from
  // `from_words` to `to_words`, a text asset's, or `text`. `after` /
  // `before`: what that message has past the passage, and before it but
  // its first paragraph, when more than a few words.
  Result<std::string> flow_source_(const Session&, const Json& args,
                                   std::string* after = nullptr,
                                   std::string* before = nullptr);
  Result<Json> objects_(ProjectId, AssetId comp, const std::string& layer);
  // The markup words and shapes go on: a still's page's (`page`), or on
  // a timeline one from `start` for `duration` seconds -- and where, as
  // the result tells it. `named`: the drawing tools', whose `layer` is
  // the one to draw on (a mask's); move_objects' is where they are.
  Result<std::pair<std::string, Json>> markup_for_(ProjectId, AssetId comp,
                                                   const Json& args,
                                                   bool named = false);
  // ---- timelines (agent-tools.cc): the model speaks SECONDS -- of the
  // timeline, of a clip's own -- and these turn them into frames.
  // A timeline the session works on (or the one named): refused for a
  // still.
  Result<AssetId> timeline_(const Session& s, const Json& args);
  Rational rate_(ProjectId, AssetId comp);
  // Each layer's place in time, resolved in seconds, by its id.
  Result<std::map<std::string, media::LayerTiming>> timings_(ProjectId,
                                                             AssetId comp);
  // What a layer is, as the timeline tools tell it: "clip", "picture",
  // "title" (a markup), "sound", or "nothing".
  std::string layer_kind_(ProjectId, const project::Layer&);
  // The rate a source's marks count at: a clip's frames, a sound's
  // milliseconds, a composition's own frames -- on a composition of
  // sound (`sound_only`), a clip's sound's milliseconds.
  Rational mark_rate_(ProjectId, AssetId source, bool sound_only);
  // A timeline's markup for `length` frames from `start`: a markup layer
  // there exactly (a title and the panel behind it share one), else a
  // new one on top.
  Result<std::string> title_layer_(ProjectId, AssetId comp,
                                   std::int64_t start, std::int64_t length);
  // A layer's volume over its `seconds`, faded in and out (keys at its
  // own frames); 1 and no fades: as it is.
  Status shape_sound_(ProjectId, AssetId comp, const std::string& layer,
                      double volume, double fade_in, double fade_out,
                      double seconds);
  // Its sound's shape (level, fades, pitch, equalizer, dips) laid again
  // over its new length -- where its keys are a shape's, not keyed by
  // hand; `was` its length before.
  Status reshape_sound_(ProjectId, AssetId comp, const std::string& layer,
                        double was, double seconds);
  // Where a timeline's last clip or picture ends, seconds (0: none).
  double shown_end_(ProjectId, AssetId comp);
  // A clip's or sound's layer cut to end at `end` (timeline seconds), its
  // fades laid again at its new end.
  Status cut_at_(ProjectId, AssetId comp, const std::string& layer,
                 double end);
  // A sound that LANDED, laid as its Landing says: cut, its volume.
  Json lay_sound_(const Landing&, AssetId comp, const std::string& layer,
                  AssetId asset);
  // A job of its own waited for (a clip's summary): false when stopped.
  bool wait_job_(Session& s, JobId job);
  // A job made for the session that lands nowhere (a separation, an
  // upscale): waited for as its pictures are, told when it is done.
  void book_(Session& s, Step& step, JobId job);
  // The enhancer's reply to `job` (assist.enhanced), taken; null when
  // none came.
  Json take_reply_(JobId job);
  // A SHOT asked for: its prompt put together (assist::shot_prompt) with
  // the names the model reads its references by, the request sent, its
  // landing booked -- one command, the step's.
  // `how`, when given: how a shot that continues carries on -- latent-
  // guided from its kept latents or its encoded tail, its latents
  // carried, or read as a reference -- for the agent to say. Latents
  // asked for that the clip did not keep, or that do not cover its end,
  // are done without, said.
  Result<JobId> submit_shot_(Session& s, const std::string& title,
                             GenerateVideoRequest req, assist::ShotText text,
                             const Landing& l, std::string* how = nullptr);
  // A shot that LANDED, laid as its Landing says: from its start, filling
  // the frame, over the clips and under the titles.
  Json lay_shot_(const Landing& l, AssetId comp, const std::string& layer,
                 AssetId asset, bool first_clip);
  // The shots held until step `after` landed: asked for now, carrying on
  // from `landed` -- or, it failed (`landed` none), failed with it, and
  // what was held after them too.
  void release_held_(Session& s, int after, std::optional<AssetId> landed,
                     const std::string& why);

  Controller&                                      _c;
  mutable std::mutex                               _mu;
  // Sessions started one at a time: one editor a project (start).
  std::mutex                                       _start_mu;
  std::map<std::string, std::shared_ptr<Session>>  _sessions;
  std::map<JobId, Landing>                         _landings;
  // Shots HELD (generate_video `continues` a shot still being made): each
  // asked for once the shot it carries on from lands.
  struct Held {
    std::string          session;
    int                  step = 0;
    int                  after = 0;
    std::string          title;
    GenerateVideoRequest req;
    assist::ShotText     text;
    Landing              landing;
  };
  std::vector<Held>                                _held;
  // Settings (<support>/agent.json).
  Json                                             _settings;
  // Events to handle on the housekeeping thread: picture landings.
  struct Pending {
    JobId       job;
    std::string kind;
    Json        data;
  };
  std::deque<Pending>                              _events;
  // The prompt enhancer's replies, by job, the newest few: the agent's
  // enhance_prompt takes its own (the person's own Enhance goes to the
  // app, and its replies age out here).
  std::deque<std::pair<JobId, Json>>               _replies;
  // Turns served for members: each session's key, until when it is
  // kept, and the folder of its saved context.
  struct Lease {
    std::chrono::steady_clock::time_point until;
    std::filesystem::path                 dir;
  };
  std::map<std::string, Lease>                     _leases;
  std::condition_variable                          _cv;
  bool                                             _stop = false;
  std::thread                                      _house;
};

}  // namespace valtz

#endif
