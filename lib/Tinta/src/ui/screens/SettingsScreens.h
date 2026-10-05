#pragma once

// Settings: a short root menu of groups, each group a page of rows for the
// profile's fields (core/profile/Profile.h). Pages are tables of Field in
// SettingsScreens.cpp; a field is one line there.

#include "app/View.h"
#include "ui/views/FormView.h"

namespace tinta::ui {

struct Field;

class SettingsScreen final : public FormView {
 public:
  using FormView::FormView;

  const char* name() const override { return "settings"; }
  const char* title() const override;

 protected:
  uint8_t rowCount() const override;
  void row(uint8_t index, RowSpec& out, char* value, size_t cap) const override;
  void activate(uint8_t index) override;
  void buildHeader(app::UiScreen& screen) override;
};

class SettingsPage final : public FormView {
 public:
  enum class Page : uint8_t { Study, Display, Time, Sleep };

  SettingsPage(app::App& app, Page page) : FormView(app), page_(page) {}

  const char* name() const override;
  const char* title() const override;

 protected:
  uint8_t rowCount() const override;
  void row(uint8_t index, RowSpec& out, char* value, size_t cap) const override;
  void step(uint8_t index, int8_t dir) override;
  void activate(uint8_t index) override;

 private:
  const Field* field(uint8_t index) const;

  Page page_;
};

}  // namespace tinta::ui
