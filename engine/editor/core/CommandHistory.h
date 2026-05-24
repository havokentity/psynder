// SPDX-License-Identifier: MIT
// Psynder — generic editor command history foundation.
//
// This is intentionally independent of player / runtime internals. Editor
// tools can either store immediate undo / redo callbacks, or store opaque
// command records and let their own dispatcher interpret the payload.

#pragma once

#include "core/Types.h"

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace psynder::editor::command_history {

enum class CommandKind : u8 {
    Callback,
    Record,
};

class Command {
   public:
    using Callback = std::function<void()>;

    Command() = default;

    static Command callback(std::string label, Callback undo, Callback redo);
    static Command record(std::string label, u64 type, std::span<const std::byte> payload);

    template <class T>
    static Command record(std::string label, u64 type, const T& payload) {
        static_assert(std::is_trivially_copyable_v<T>, "command record payloads must be trivially copyable");
        const auto* bytes = reinterpret_cast<const std::byte*>(&payload);
        return record(std::move(label), type, std::span<const std::byte>{bytes, sizeof(T)});
    }

    CommandKind kind() const noexcept { return kind_; }
    std::string_view label() const noexcept { return label_; }

    u64 record_type() const noexcept { return record_type_; }
    std::span<const std::byte> payload() const noexcept { return payload_; }

    bool has_undo_callback() const noexcept { return static_cast<bool>(undo_); }
    bool has_redo_callback() const noexcept { return static_cast<bool>(redo_); }

    void undo() const;
    void redo() const;

   private:
    CommandKind kind_ = CommandKind::Record;
    std::string label_;
    u64 record_type_ = 0;
    std::vector<std::byte> payload_;
    Callback undo_;
    Callback redo_;
};

class History {
   public:
    explicit History(usize max_depth = 0);

    void push(Command command);
    void push_callback(std::string label, Command::Callback undo, Command::Callback redo);
    void push_record(std::string label, u64 type, std::span<const std::byte> payload);

    template <class T>
    void push_record(std::string label, u64 type, const T& payload) {
        push(Command::record(std::move(label), type, payload));
    }

    bool undo(Command& out);
    bool redo(Command& out);

    void clear() noexcept;
    void set_max_depth(usize max_depth);

    bool can_undo() const noexcept { return !done_.empty(); }
    bool can_redo() const noexcept { return !redo_.empty(); }
    usize size() const noexcept { return done_.size(); }
    usize redo_size() const noexcept { return redo_.size(); }
    usize max_depth() const noexcept { return max_depth_; }

    std::string_view undo_label() const noexcept;
    std::string_view redo_label() const noexcept;

   private:
    void trim_to_depth();

    std::vector<Command> done_;
    std::vector<Command> redo_;
    usize max_depth_ = 0;  // 0 = unlimited
};

}  // namespace psynder::editor::command_history
