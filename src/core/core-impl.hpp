#ifndef WF_CORE_CORE_IMPL_HPP
#define WF_CORE_CORE_IMPL_HPP

#include "core/plugin-loader.hpp"
#include "wayfire/core.hpp"
#include "wayfire/scene-input.hpp"
#include "wayfire/scene.hpp"
#include "wayfire/util.hpp"
#include <wayfire/nonstd/wlroots-full.hpp>

#include <set>
#include <unordered_map>

struct wayfire_shell;

namespace wf
{
class seat_t;
class input_manager_t;
class input_method_relay;
class compositor_core_impl_t : public compositor_core_t
{
  public:
    /**
     * When we get a request for setting CSD, the view might not have been
     * created. So, we store all requests in core, and the views pick this
     * information when they are created
     */
    std::unordered_map<wlr_surface*, uint32_t> uses_csd;

    wlr_egl *egl;
    wlr_compositor *compositor;

    std::unique_ptr<wf::input_manager_t> input;
    std::unique_ptr<input_method_relay> im_relay;
    std::unique_ptr<plugin_manager_t> plugin_mgr;

    /**
     * Initialize the compositor core.
     * Called only by main().
     */
    virtual void init();

    /**
     * Finish initialization of core after the backend has started.
     * Called only by main().
     */
    virtual void post_init();

    wayfire_shell *wf_shell;

    /**
     * Remove a view from the compositor list. This is called when the view's
     * keep_count reaches zero for the first time after its creation.
     */
    virtual void erase_view(wayfire_view view);

    /**
     * Find a view by its stringified ID.
     * @return nullptr if no such view exists.
     */
    virtual wayfire_view find_view(const std::string& id);

    static compositor_core_impl_t& get();

    wlr_seat *get_current_seat() override;
    void warp_cursor(wf::pointf_t pos) override;
    void transfer_grab(wf::scene::node_ptr node, bool retain_pressed_state) override;
    void set_cursor(std::string name) override;
    void unhide_cursor() override;
    void hide_cursor() override;

    wf::pointf_t get_cursor_position() override;
    wf::pointf_t get_touch_position(int id) override;
    const wf::touch::gesture_state_t& get_touch_state() override;

    wf::scene::node_ptr get_cursor_focus() override;
    wf::scene::node_ptr get_touch_focus() override;

    void add_touch_gesture(
        nonstd::observer_ptr<wf::touch::gesture_t> gesture) override;
    void rem_touch_gesture(
        nonstd::observer_ptr<wf::touch::gesture_t> gesture) override;

    std::vector<nonstd::observer_ptr<wf::input_device_t>> get_input_devices()
    override;
    virtual wlr_cursor *get_wlr_cursor() override;

    void add_view(std::unique_ptr<wf::view_interface_t> view) override;
    std::vector<wayfire_view> get_all_views() override;
    void focus_view(wayfire_view win) override;
    void move_view_to_output(wayfire_view v, wf::output_t *new_output,
        bool reconfigure) override;

    void focus_output(wf::output_t *o) override;
    wf::output_t *get_active_output() override;
    std::string get_xwayland_display() override;
    pid_t run(std::string command) override;
    void shutdown() override;
    compositor_state_t get_current_state() override;
    const std::shared_ptr<scene::root_node_t>& scene() final;

  protected:
    wf::wl_listener_wrapper decoration_created;
    wf::wl_listener_wrapper xdg_decoration_created;
    wf::wl_listener_wrapper vkbd_created;
    wf::wl_listener_wrapper vptr_created;
    wf::wl_listener_wrapper input_inhibit_activated;
    wf::wl_listener_wrapper input_inhibit_deactivated;
    wf::wl_listener_wrapper pointer_constraint_added;
    wf::wl_listener_wrapper idle_inhibitor_created;
    wf::wl_listener_wrapper keyboard_inhibit_new;

    wf::output_t *active_output = nullptr;
    std::vector<std::unique_ptr<wf::view_interface_t>> views;
    std::unordered_map<std::string, wayfire_view> id_to_view;

    std::shared_ptr<scene::root_node_t> scene_root;

    compositor_state_t state = compositor_state_t::UNKNOWN;
    compositor_core_impl_t();
    virtual ~compositor_core_impl_t();
};

compositor_core_impl_t& get_core_impl();
}


#endif /* end of include guard: WF_CORE_CORE_IMPL_HPP */
