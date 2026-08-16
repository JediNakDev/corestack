#include <stddef.h>

#include "libtetrisui/tetrisui.h"
#include "ballotu/mock.h"
#include "ballotu/screens.h"

int main(void) {
  mock_init();
  tetrisui_init();
  tetrisui_set_status("ballotu", "(not logged in)", "");

  if (screen_login()) {
    const char *items[] = {"Join election (UC-2)", "Cast vote (UC-3)", "Update vote (UC-4)",
                            "View results (UC-5)", "Check your vote (UC-6)", "Quit"};
    for (;;) {
      int sel = tetrisui_menu("ballotu - voter menu", items, 6, NULL);
      if (sel < 0 || sel == 5) break;
      switch (sel) {
        case 0: screen_join_election(); break;
        case 1: screen_cast_vote(); break;
        case 2: screen_update_vote(); break;
        case 3: screen_view_results(); break;
        case 4: screen_check_vote(); break;
      }
    }
  }

  tetrisui_shutdown();
  return 0;
}
