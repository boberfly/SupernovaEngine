#pragma once

#include "WidgetWindow.hpp"
#include "TextEditor.h"
#include "entt/signal/emitter.hpp"
#include <filesystem>

class ScriptEditor final : public WidgetWindow,
                           private entt::emitter<ScriptEditor> {
  friend class entt::emitter<ScriptEditor>;

public:
  ScriptEditor() = default;
  ScriptEditor(const ScriptEditor &) = delete;

  ScriptEditor &operator=(const ScriptEditor &) = delete;

  using entt::emitter<ScriptEditor>::on;

  struct RunScriptRequest {
    std::string code;
  };

  void newScript();

  bool open(const std::filesystem::path &);
  [[nodiscard]] bool contains(const std::filesystem::path &) const;

  [[nodiscard]] auto size() const { return m_scripts.size(); }

  void show(const char *name, bool *open) override;

private:
  struct Entry {
    explicit Entry(const std::filesystem::path & = {});

    [[nodiscard]] bool isOnDisk() const { return !path.empty(); }
    [[nodiscard]] bool isChanged() const {
      return textEditor.GetUndoIndex() != undoIndex;
    }

    void load();

    void save();
    void saveAs(const std::filesystem::path &p);

    std::filesystem::path path;
    int32_t undoIndex{0};

    TextEditor textEditor;
  };

  Entry *_getActiveEntry();
  void _runScript(const Entry &);
  void _removeScriptAt(std::size_t index);

private:
  std::vector<Entry> m_scripts;
  std::optional<std::size_t> m_activeScriptId;

  std::optional<std::size_t> m_junk;
};
