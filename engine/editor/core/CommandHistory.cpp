// SPDX-License-Identifier: MIT
// Psynder — generic editor command history foundation.

#include "CommandHistory.h"

namespace psynder::editor::command_history {

Command Command::callback(std::string label, Callback undo, Callback redo) {
    Command command;
    command.kind_ = CommandKind::Callback;
    command.label_ = std::move(label);
    command.undo_ = std::move(undo);
    command.redo_ = std::move(redo);
    return command;
}

Command Command::record(std::string label, u64 type, std::span<const std::byte> payload) {
    Command command;
    command.kind_ = CommandKind::Record;
    command.label_ = std::move(label);
    command.record_type_ = type;
    command.payload_.assign(payload.begin(), payload.end());
    return command;
}

void Command::undo() const {
    if (undo_) {
        undo_();
    }
}

void Command::redo() const {
    if (redo_) {
        redo_();
    }
}

History::History(usize max_depth) : max_depth_(max_depth) {}

void History::push(Command command) {
    done_.push_back(std::move(command));
    redo_.clear();
    trim_to_depth();
}

void History::push_callback(std::string label, Command::Callback undo, Command::Callback redo) {
    push(Command::callback(std::move(label), std::move(undo), std::move(redo)));
}

void History::push_record(std::string label, u64 type, std::span<const std::byte> payload) {
    push(Command::record(std::move(label), type, payload));
}

bool History::undo(Command& out) {
    if (done_.empty()) {
        return false;
    }

    out = done_.back();
    out.undo();
    redo_.push_back(std::move(done_.back()));
    done_.pop_back();
    return true;
}

bool History::redo(Command& out) {
    if (redo_.empty()) {
        return false;
    }

    out = redo_.back();
    out.redo();
    done_.push_back(std::move(redo_.back()));
    redo_.pop_back();
    trim_to_depth();
    return true;
}

void History::clear() noexcept {
    done_.clear();
    redo_.clear();
}

void History::set_max_depth(usize max_depth) {
    max_depth_ = max_depth;
    trim_to_depth();
}

std::string_view History::undo_label() const noexcept {
    return done_.empty() ? std::string_view{} : done_.back().label();
}

std::string_view History::redo_label() const noexcept {
    return redo_.empty() ? std::string_view{} : redo_.back().label();
}

void History::trim_to_depth() {
    if (max_depth_ == 0 || done_.size() <= max_depth_) {
        return;
    }

    const usize trim_count = done_.size() - max_depth_;
    done_.erase(done_.begin(), done_.begin() + static_cast<std::ptrdiff_t>(trim_count));
}

}  // namespace psynder::editor::command_history
