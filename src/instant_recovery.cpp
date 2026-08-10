#include "instant_recovery.h"

#include "safe_output.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>

namespace instant_recovery {
namespace {

std::string escape(const std::string_view value) {
    std::ostringstream out;
    for (const unsigned char c : value) {
        switch (c) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) out << "\\u" << std::hex << std::setw(4)
                                  << std::setfill('0') << static_cast<int>(c);
                else out << static_cast<char>(c);
        }
    }
    return out.str();
}

std::string field(const std::string &json, const std::string_view name) {
    const std::string marker = "\"" + std::string(name) + "\"";
    auto at = json.find(marker);
    if (at == std::string::npos) return {};
    at = json.find(':', at + marker.size());
    if (at == std::string::npos) return {};
    at = json.find('"', at + 1);
    if (at == std::string::npos) return {};
    std::string result;
    bool escaped = false;
    for (++at; at < json.size(); ++at) {
        const char c = json[at];
        if (escaped) {
            if (c == 'n') result += '\n';
            else if (c == 'r') result += '\r';
            else if (c == 't') result += '\t';
            else result += c;
            escaped = false;
        } else if (c == '\\') escaped = true;
        else if (c == '"') return result;
        else result += c;
    }
    throw std::runtime_error("unterminated recovery job JSON string");
}

bool boolean_field(const std::string &json, const std::string_view name) {
    const std::string marker = "\"" + std::string(name) + "\"";
    const auto at = json.find(marker);
    if (at == std::string::npos) return false;
    const auto colon = json.find(':', at + marker.size());
    return colon != std::string::npos &&
        json.substr(colon + 1, 8).find("true") != std::string::npos;
}

} // namespace

std::string_view phase_name(const Phase phase) noexcept {
    switch (phase) {
        case Phase::ValidatingPlaylist: return "validating_playlist";
        case Phase::PreparingJob: return "preparing_job";
        case Phase::Downloading: return "downloading";
        case Phase::Scanning: return "scanning";
        case Phase::SelectingSet: return "selecting_set";
        case Phase::ReadyToRecover: return "ready_to_recover";
        case Phase::Recovering: return "recovering";
        case Phase::CheckingFinalSha: return "checking_final_sha";
        case Phase::RecoveredExact: return "recovered_exact";
        case Phase::NeedsAttention: return "needs_attention";
        case Phase::Failed: return "failed";
        case Phase::Cancelled: return "cancelled";
    }
    return "failed";
}

Phase parse_phase(const std::string_view value) {
    for (const auto phase : {Phase::ValidatingPlaylist, Phase::PreparingJob,
             Phase::Downloading, Phase::Scanning, Phase::SelectingSet,
             Phase::ReadyToRecover, Phase::Recovering,
             Phase::CheckingFinalSha, Phase::RecoveredExact,
             Phase::NeedsAttention, Phase::Failed, Phase::Cancelled})
        if (phase_name(phase) == value) return phase;
    throw std::invalid_argument("unknown instant recovery phase");
}

Selection select_single_complete_set(
    const std::vector<SetCandidate> &candidates) {
    std::vector<const SetCandidate *> recoverable;
    for (const auto &candidate : candidates) {
        if (!candidate.set_id.empty() && candidate.expected_parts != 0 &&
            candidate.exact_parts == candidate.expected_parts &&
            candidate.corrupt_count == 0 && candidate.conflict_count == 0)
            recoverable.push_back(&candidate);
    }
    if (recoverable.size() == 1)
        return {SelectionStatus::Selected, recoverable.front()->set_id,
                "One complete Video Set was selected."};
    if (recoverable.empty())
        return {SelectionStatus::NoneRecoverable, std::nullopt,
                "No complete recoverable Video Set was found."};
    return {SelectionStatus::MultipleRecoverable, std::nullopt,
            "Multiple complete Video Sets were found; choose one explicitly."};
}

bool may_auto_recover(const JobState &job, const bool output_exists,
                      const Selection &selection) noexcept {
    return job.explicit_auto_recover && !output_exists &&
           selection.status == SelectionStatus::Selected &&
           selection.set_id.has_value();
}

std::string make_job_id() {
    std::array<unsigned char, 16> bytes{};
    std::random_device random;
    for (auto &byte : bytes) byte = static_cast<unsigned char>(random());
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);
    std::ostringstream out;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out << '-';
        out << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(bytes[i]);
    }
    return out.str();
}

std::filesystem::path default_jobs_root() {
    if (const char *overrideRoot = std::getenv(
            "VIDSTOREX_RECOVERY_JOBS_ROOT");
        overrideRoot && *overrideRoot)
        return std::filesystem::u8path(overrideRoot);
#ifdef _WIN32
    if (const char *local = std::getenv("LOCALAPPDATA"); local && *local)
        return std::filesystem::u8path(local) / "VidStoreX" / "RecoveryJobs";
#endif
    return std::filesystem::temp_directory_path() / "VidStoreX" /
           "RecoveryJobs";
}

void initialize_job_directories(const std::filesystem::path &job_root) {
    std::filesystem::create_directories(job_root / "returned");
    std::filesystem::create_directories(job_root / "logs");
}

void write_job_state_atomic(const std::filesystem::path &path,
                            const JobState &state) {
    if (state.job_id.empty() || state.playlist_url.empty())
        throw std::invalid_argument("recovery job identity and playlist are required");
    std::filesystem::create_directories(path.parent_path());
    SafeOutputFile safe(path);
    std::ofstream out(safe.partial_path(), std::ios::binary);
    if (!out) throw std::runtime_error("could not open recovery job state");
    out << "{\n  \"schema\": \"vidstorex.instant_recovery_job\",\n"
        << "  \"version\": 1,\n  \"job_id\": \"" << escape(state.job_id)
        << "\",\n  \"playlist_url\": \"" << escape(state.playlist_url)
        << "\",\n  \"output_directory\": \"" << escape(state.output_directory)
        << "\",\n  \"selected_set_id\": \"" << escape(state.selected_set_id)
        << "\",\n  \"phase\": \"" << phase_name(state.phase)
        << "\",\n  \"explicit_auto_recover\": "
        << (state.explicit_auto_recover ? "true" : "false")
        << ",\n  \"final_output_path\": \"" << escape(state.final_output_path)
        << "\",\n  \"final_sha256\": \"" << escape(state.final_sha256)
        << "\",\n  \"final_sha_exact\": "
        << (state.final_sha_exact ? "true" : "false")
        << ",\n  \"updated_at\": " << state.updated_at_epoch_seconds << "\n}\n";
    out.close();
    if (!out) throw std::runtime_error("could not write recovery job state");
    safe.commit();
}

JobState read_job_state(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    const std::string json((std::istreambuf_iterator<char>(in)), {});
    if (json.find("\"schema\": \"vidstorex.instant_recovery_job\"") ==
            std::string::npos ||
        json.find("\"version\": 1") == std::string::npos)
        throw std::runtime_error("unsupported recovery job state");
    JobState state;
    state.job_id = field(json, "job_id");
    state.playlist_url = field(json, "playlist_url");
    state.output_directory = field(json, "output_directory");
    state.selected_set_id = field(json, "selected_set_id");
    state.phase = parse_phase(field(json, "phase"));
    state.explicit_auto_recover = boolean_field(json, "explicit_auto_recover");
    state.final_output_path = field(json, "final_output_path");
    state.final_sha256 = field(json, "final_sha256");
    state.final_sha_exact = boolean_field(json, "final_sha_exact");
    if (state.job_id.empty() || state.playlist_url.empty())
        throw std::runtime_error("incomplete recovery job state");
    return state;
}

} // namespace instant_recovery
