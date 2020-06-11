#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <assert.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <czmq.h>
#include "common/util.h"
#include "common/timing.h"
#include "common/swaglog.h"
#include "common/touch.h"
#include "common/visionimg.h"
#include "common/params.h"
#include "ui.hpp"

static int last_brightness = -1;
static void set_brightness(UIState *s, int brightness) {
  if (last_brightness != brightness && (s->awake || brightness == 0)) {
    FILE *f = fopen("/sys/class/leds/lcd-backlight/brightness", "wb");
    if (f != NULL) {
      fprintf(f, "%d", brightness);
      fclose(f);
      last_brightness = brightness;
    }
  }
}

int event_processing_enabled = -1;
static void enable_event_processing(bool yes) {
  if (event_processing_enabled != 1 && yes) {
    system("service call window 18 i32 1");  // enable event processing
    event_processing_enabled = 1;
  } else if (event_processing_enabled != 0 && !yes) {
    system("service call window 18 i32 0");  // disable event processing
    event_processing_enabled = 0;
  }
}

static void set_awake(UIState *s, bool awake) {
#ifdef QCOM
  if (awake) {
    // 30 second timeout at 30 fps
    s->awake_timeout = 30*30;
  }
  if (s->awake != awake) {
    s->awake = awake;

    // TODO: replace command_awake and command_sleep with direct calls to android
    if (awake) {
      LOGW("awake normal");
      framebuffer_set_power(s->fb, HWC_POWER_MODE_NORMAL);
      enable_event_processing(true);
    } else {
      LOGW("awake off");
      set_brightness(s, 0);
      framebuffer_set_power(s->fb, HWC_POWER_MODE_OFF);
      enable_event_processing(false);
    }
  }
#else
  // computer UI doesn't sleep
  s->awake = true;
#endif
}

static void update_offroad_layout_state(UIState *s) {
  capnp::MallocMessageBuilder msg;
  auto event = msg.initRoot<cereal::Event>();
  event.setLogMonoTime(nanos_since_boot());
  auto layout = event.initUiLayoutState();
  layout.setActiveApp(s->active_app);
  layout.setSidebarCollapsed(s->scene.uilayout_sidebarcollapsed);
  s->pm->send("offroadLayout", msg);
  LOGD("setting active app to %d with sidebar %d", (int)s->active_app, s->scene.uilayout_sidebarcollapsed);
}

static void navigate_to_settings(UIState *s) {
#ifdef QCOM
  s->active_app = cereal::UiLayoutState::App::SETTINGS;
  update_offroad_layout_state(s);
#else
  // computer UI doesn't have offroad settings
#endif
}

static void navigate_to_home(UIState *s) {
#ifdef QCOM
  if (s->started) {
    s->active_app = cereal::UiLayoutState::App::NONE;
  } else {
    s->active_app = cereal::UiLayoutState::App::HOME;
  }
  update_offroad_layout_state(s);
#else
  // computer UI doesn't have offroad home
#endif
}

static void handle_sidebar_touch(UIState *s, int touch_x, int touch_y) {
  if (!s->scene.uilayout_sidebarcollapsed && touch_x <= sbr_w) {
    if (touch_x >= settings_btn_x && touch_x < (settings_btn_x + settings_btn_w)
      && touch_y >= settings_btn_y && touch_y < (settings_btn_y + settings_btn_h)) {
      navigate_to_settings(s);
    }
    if (touch_x >= home_btn_x && touch_x < (home_btn_x + home_btn_w)
      && touch_y >= home_btn_y && touch_y < (home_btn_y + home_btn_h)) {
      navigate_to_home(s);
      if (s->started) {
        s->scene.uilayout_sidebarcollapsed = true;
        update_offroad_layout_state(s);
      }
    }
  }
}

static void handle_driver_view_touch(UIState *s, int touch_x, int touch_y) {
  int err = write_db_value("IsDriverViewEnabled", "0", 1);
}

static void handle_vision_touch(UIState *s, int touch_x, int touch_y) {
  if (s->started && (touch_x >= s->scene.ui_viz_rx - bdr_s)
    && (s->active_app != cereal::UiLayoutState::App::SETTINGS)) {
    if (!s->scene.frontview) {
      s->scene.uilayout_sidebarcollapsed = !s->scene.uilayout_sidebarcollapsed;
    } else {
      handle_driver_view_touch(s, touch_x, touch_y);
    }
    update_offroad_layout_state(s);
  }
}

volatile sig_atomic_t do_exit = 0;
static void set_do_exit(int sig) {
  do_exit = 1;
}

static void read_param_bool(bool* param, const char* param_name, bool persistent_param = false) {
  char *s;
  const int result = read_db_value(param_name, &s, NULL, persistent_param);
  if (result == 0) {
    *param = s[0] == '1';
    free(s);
  }
}

static int read_param_float(float* param, const char* param_name, bool persistent_param = false) {
  char *s;
  const int result = read_db_value(param_name, &s, NULL, persistent_param);
  if (result == 0) {
    *param = strtod(s, NULL);
    free(s);
  }
  return result;
}

static int read_param_uint64(uint64_t* dest, const char* param_name, bool persistent_param = false) {
  char *s;
  const int result = read_db_value(param_name, &s, NULL, persistent_param);
  if (result == 0) {
    *dest = strtoull(s, NULL, 0);
    free(s);
  }
  return result;
}

static void read_param_bool_timeout(bool* param, const char* param_name, int* timeout, bool persistent_param = false) {
  if (*timeout > 0){
    (*timeout)--;
  } else {
    read_param_bool(param, param_name, persistent_param);
    *timeout = 2 * UI_FREQ; // 0.5Hz
  }
}

static void read_param_float_timeout(float* param, const char* param_name, int* timeout, bool persistent_param = false) {
  if (*timeout > 0){
    (*timeout)--;
  } else {
    read_param_float(param, param_name, persistent_param);
    *timeout = 2 * UI_FREQ; // 0.5Hz
  }
}

static int read_param_uint64_timeout(uint64_t* dest, const char* param_name, int* timeout, bool persistent_param = false) {
  if (*timeout > 0){
    (*timeout)--;
    return -1;
  } else {
    *timeout = 2 * UI_FREQ; // 0.5Hz
    return read_param_uint64(dest, param_name, persistent_param);
  }
}

static int write_param_float(float param, const char* param_name, bool persistent_param = false) {
  char s[16];
  int size = snprintf(s, sizeof(s), "%f", param);
  return write_db_value(param_name, s, MIN(size, sizeof(s)), persistent_param);
}

static void update_offroad_layout_timeout(UIState *s, int* timeout) {
  if (*timeout > 0) {
    (*timeout)--;
  } else {
    update_offroad_layout_state(s);
    *timeout = 2 * UI_FREQ;
  }
}

static void ui_init(UIState *s) {

  pthread_mutex_init(&s->lock, NULL);
  s->sm = new SubMaster({"model", "controlsState", "uiLayoutState", "liveCalibration", "radarState", "thermal",
                         "health", "ubloxGnss", "driverState", "dMonitoringState", "offroadLayout"
#ifdef SHOW_SPEEDLIMIT
                                    , "liveMapData"
#endif
  });
  s->pm = new PubMaster({"offroadLayout"});

  s->ipc_fd = -1;

  // init display
  s->fb = framebuffer_init("ui", 0, true, &s->fb_w, &s->fb_h);
  assert(s->fb);

  set_awake(s, true);

  s->model_changed = false;
  s->livempc_or_radarstate_changed = false;

  ui_nvg_init(s);
}

static void ui_init_vision(UIState *s, const VisionStreamBufs back_bufs,
                           int num_back_fds, const int *back_fds,
                           const VisionStreamBufs front_bufs, int num_front_fds,
                           const int *front_fds) {
  const VisionUIInfo ui_info = back_bufs.buf_info.ui_info;

  assert(num_back_fds == UI_BUF_COUNT);
  assert(num_front_fds == UI_BUF_COUNT);

  vipc_bufs_load(s->bufs, &back_bufs, num_back_fds, back_fds);
  vipc_bufs_load(s->front_bufs, &front_bufs, num_front_fds, front_fds);

  s->cur_vision_idx = -1;
  s->cur_vision_front_idx = -1;

  s->scene = (UIScene){
      .frontview = getenv("FRONTVIEW") != NULL,
      .fullview = getenv("FULLVIEW") != NULL,
      .transformed_width = ui_info.transformed_width,
      .transformed_height = ui_info.transformed_height,
      .front_box_x = ui_info.front_box_x,
      .front_box_y = ui_info.front_box_y,
      .front_box_width = ui_info.front_box_width,
      .front_box_height = ui_info.front_box_height,
      .world_objects_visible = false,  // Invisible until we receive a calibration message.
      .gps_planner_active = false,
  };

  s->rgb_width = back_bufs.width;
  s->rgb_height = back_bufs.height;
  s->rgb_stride = back_bufs.stride;
  s->rgb_buf_len = back_bufs.buf_len;

  s->rgb_front_width = front_bufs.width;
  s->rgb_front_height = front_bufs.height;
  s->rgb_front_stride = front_bufs.stride;
  s->rgb_front_buf_len = front_bufs.buf_len;

  s->rgb_transform = (mat4){{
    2.0f/s->rgb_width, 0.0f, 0.0f, -1.0f,
    0.0f, 2.0f/s->rgb_height, 0.0f, -1.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f,
  }};

  read_param_float(&s->speed_lim_off, "SpeedLimitOffset");
  read_param_bool(&s->is_metric, "IsMetric");
  read_param_bool(&s->longitudinal_control, "LongitudinalControl");
  read_param_bool(&s->limit_set_speed, "LimitSetSpeed");

  // Set offsets so params don't get read at the same time
  s->longitudinal_control_timeout = UI_FREQ / 3;
  s->is_metric_timeout = UI_FREQ / 2;
  s->limit_set_speed_timeout = UI_FREQ;
}

static void read_path(PathData& p, const cereal::ModelData::PathData::Reader &pathp) {
  p = {};

  p.prob = pathp.getProb();
  p.std = pathp.getStd();

  auto polyp = pathp.getPoly();
  for (int i = 0; i < POLYFIT_DEGREE; i++) {
    p.poly[i] = polyp[i];
  }

  // Compute points locations
  for (int i = 0; i < MODEL_PATH_DISTANCE; i++) {
    p.points[i] = p.poly[0] * (i*i*i) + p.poly[1] * (i*i)+ p.poly[2] * i + p.poly[3];
  }
}

static void read_model(ModelData &d, const cereal::ModelData::Reader &model) {
  d = {};
  read_path(d.path, model.getPath());
  read_path(d.left_lane, model.getLeftLane());
  read_path(d.right_lane, model.getRightLane());
  auto leadd = model.getLead();
  d.lead = (LeadData){
      .dist = leadd.getDist(), .prob = leadd.getProb(), .std = leadd.getStd(),
  };
}

static void update_status(UIState *s, int status) {
  if (s->status != status) {
    s->status = status;
  }
}

void handle_message(UIState *s, SubMaster &sm) {
  UIScene &scene = s->scene;
  if (s->started && sm.updated("controlsState")) {
    auto event = sm["controlsState"];
    auto data = event.getControlsState();
    s->controls_timeout = 1 * UI_FREQ;
    scene.frontview = data.getRearViewCam();
    if (!scene.frontview){ s->controls_seen = true; }

    if (data.getVCruise() != scene.v_cruise) {
      scene.v_cruise_update_ts = event.getLogMonoTime();
    }
    scene.v_cruise = data.getVCruise();
    scene.v_ego = data.getVEgo();
    scene.curvature = data.getCurvature();
    scene.engaged = data.getEnabled();
    scene.engageable = data.getEngageable();
    scene.gps_planner_active = data.getGpsPlannerActive();
    scene.monitoring_active = data.getDriverMonitoringOn();
    scene.decel_for_model = data.getDecelForModel();
    auto alert_sound = data.getAlertSound();
    const auto sound_none = cereal::CarControl::HUDControl::AudibleAlert::NONE;
    if (alert_sound != s->alert_sound){
      if (s->alert_sound != sound_none){
        stop_alert_sound(s->alert_sound);
      }
      if (alert_sound != sound_none){
        play_alert_sound(alert_sound);
        s->alert_type = data.getAlertType();
      }
      s->alert_sound = alert_sound;
    }
    scene.alert_text1 = data.getAlertText1();
    scene.alert_text2 = data.getAlertText2();
    scene.alert_ts = event.getLogMonoTime();
    scene.alert_size = data.getAlertSize();
    auto alertStatus = data.getAlertStatus();
    if (alertStatus == cereal::ControlsState::AlertStatus::USER_PROMPT) {
      update_status(s, STATUS_WARNING);
    } else if (alertStatus == cereal::ControlsState::AlertStatus::CRITICAL) {
      update_status(s, STATUS_ALERT);
    } else{
      update_status(s, scene.engaged ? STATUS_ENGAGED : STATUS_DISENGAGED);
    }

    scene.alert_blinkingrate = data.getAlertBlinkingRate();
    if (scene.alert_blinkingrate > 0.) {
      if (s->alert_blinked) {
        if (s->alert_blinking_alpha > 0.0 && s->alert_blinking_alpha < 1.0) {
          s->alert_blinking_alpha += (0.05*scene.alert_blinkingrate);
        } else {
          s->alert_blinked = false;
        }
      } else {
        if (s->alert_blinking_alpha > 0.25) {
          s->alert_blinking_alpha -= (0.05*scene.alert_blinkingrate);
        } else {
          s->alert_blinking_alpha += 0.25;
          s->alert_blinked = true;
        }
      }
    }
  }
  if (sm.updated("radarState")) {
    auto data = sm["radarState"].getRadarState();

    auto leaddatad = data.getLeadOne();
    scene.lead_status = leaddatad.getStatus();
    scene.lead_d_rel = leaddatad.getDRel();
    scene.lead_y_rel = leaddatad.getYRel();
    scene.lead_v_rel = leaddatad.getVRel();
    auto leaddatad2 = data.getLeadTwo();
    scene.lead_status2 = leaddatad2.getStatus();
    scene.lead_d_rel2 = leaddatad2.getDRel();
    scene.lead_y_rel2 = leaddatad2.getYRel();
    scene.lead_v_rel2 = leaddatad2.getVRel();
    s->livempc_or_radarstate_changed = true;
  }
  if (sm.updated("liveCalibration")) {
    scene.world_objects_visible = true;
    auto extrinsicl = sm["liveCalibration"].getLiveCalibration().getExtrinsicMatrix();
    for (int i = 0; i < 3 * 4; i++) {
      scene.extrinsic_matrix.v[i] = extrinsicl[i];
    }
  }
  if (sm.updated("model")) {
    read_model(scene.model, sm["model"].getModel());
    s->model_changed = true;
  }
  // else if (which == cereal::Event::LIVE_MPC) {
  //   auto data = event.getLiveMpc();
  //   auto x_list = data.getX();
  //   auto y_list = data.getY();
  //   for (int i = 0; i < 50; i++){
  //     scene.mpc_x[i] = x_list[i];
  //     scene.mpc_y[i] = y_list[i];
  //   }
  //   s->livempc_or_radarstate_changed = true;
  // }
  if (sm.updated("uiLayoutState")) {
    auto data = sm["uiLayoutState"].getUiLayoutState();
    s->active_app = data.getActiveApp();
    scene.uilayout_sidebarcollapsed = data.getSidebarCollapsed();
    if (data.getMockEngaged() != scene.uilayout_mockengaged) {
      scene.uilayout_mockengaged = data.getMockEngaged();
    }
  }
#ifdef SHOW_SPEEDLIMIT
  if (sm.updated("liveMapData")) {
    scene.map_valid = sm["liveMapData"].getLiveMapData().getMapValid();
  }
#endif
  if (sm.updated("thermal")) {
    auto data = sm["thermal"].getThermal();
    scene.networkType = data.getNetworkType();
    scene.networkStrength = data.getNetworkStrength();
    scene.batteryPercent = data.getBatteryPercent();
    scene.batteryCharging = data.getBatteryStatus() == "Charging";
    scene.freeSpace = data.getFreeSpace();
    scene.thermalStatus = data.getThermalStatus();
    scene.paTemp = data.getPa0();

    s->thermal_started = data.getStarted();
  }
  if (sm.updated("ubloxGnss")) {
    auto data = sm["ubloxGnss"].getUbloxGnss();
    if (data.which() == cereal::UbloxGnss::MEASUREMENT_REPORT) {
      scene.satelliteCount = data.getMeasurementReport().getNumMeas();
    }
  }
  if (sm.updated("health")) {
    scene.hwType = sm["health"].getHealth().getHwType();
    s->hardware_timeout = 5*30; // 5 seconds at 30 fps
  }
  if (sm.updated("driverState")) {
    auto data = sm["driverState"].getDriverState();
    scene.face_prob = data.getFaceProb();
    auto fxy_list = data.getFacePosition();
    scene.face_x = fxy_list[0];
    scene.face_y = fxy_list[1];
  }
  if (sm.updated("dMonitoringState")) {
    auto data = sm["dMonitoringState"].getDMonitoringState();
    scene.is_rhd = data.getIsRHD();
    scene.awareness_status = data.getAwarenessStatus();
    s->preview_started = data.getIsPreview();
  }

  s->started = s->thermal_started || s->preview_started ;
  // Handle onroad/offroad transition
  if (!s->started) {
    if (s->status != STATUS_STOPPED) {
      update_status(s, STATUS_STOPPED);
      s->alert_sound_timeout = 0;
      s->vision_seen = false;
      s->controls_seen = false;
      s->active_app = cereal::UiLayoutState::App::HOME;
      update_offroad_layout_state(s);
    }
  } else if (s->status == STATUS_STOPPED) {
    update_status(s, STATUS_DISENGAGED);

    s->active_app = cereal::UiLayoutState::App::NONE;
    update_offroad_layout_state(s);
  }
}

static void check_messages(UIState *s) {
  while (true) {
    if (s->sm->update(0) == 0)
      break;
    handle_message(s, *(s->sm));
  }
}

static void ui_update(UIState *s) {
  int err;

  if (s->vision_connect_firstrun) {
    // cant run this in connector thread because opengl.
    // do this here for now in lieu of a run_on_main_thread event

    for (int i=0; i<UI_BUF_COUNT; i++) {
      if(s->khr[i] != 0) {
        visionimg_destroy_gl(s->khr[i], s->priv_hnds[i]);
        glDeleteTextures(1, &s->frame_texs[i]);
      }

      VisionImg img = {
        .fd = s->bufs[i].fd,
        .format = VISIONIMG_FORMAT_RGB24,
        .width = s->rgb_width,
        .height = s->rgb_height,
        .stride = s->rgb_stride,
        .bpp = 3,
        .size = s->rgb_buf_len,
      };
      #ifndef QCOM
        s->priv_hnds[i] = s->bufs[i].addr;
      #endif
      s->frame_texs[i] = visionimg_to_gl(&img, &s->khr[i], &s->priv_hnds[i]);

      glBindTexture(GL_TEXTURE_2D, s->frame_texs[i]);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

      // BGR
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
    }

    for (int i=0; i<UI_BUF_COUNT; i++) {
      if(s->khr_front[i] != 0) {
        visionimg_destroy_gl(s->khr_front[i], s->priv_hnds_front[i]);
        glDeleteTextures(1, &s->frame_front_texs[i]);
      }

      VisionImg img = {
        .fd = s->front_bufs[i].fd,
        .format = VISIONIMG_FORMAT_RGB24,
        .width = s->rgb_front_width,
        .height = s->rgb_front_height,
        .stride = s->rgb_front_stride,
        .bpp = 3,
        .size = s->rgb_front_buf_len,
      };
      #ifndef QCOM
        s->priv_hnds_front[i] = s->bufs[i].addr;
      #endif
      s->frame_front_texs[i] = visionimg_to_gl(&img, &s->khr_front[i], &s->priv_hnds_front[i]);

      glBindTexture(GL_TEXTURE_2D, s->frame_front_texs[i]);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

      // BGR
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
    }

    assert(glGetError() == GL_NO_ERROR);

    s->scene.uilayout_sidebarcollapsed = true;
    update_offroad_layout_state(s);
    s->scene.ui_viz_rx = (box_x-sbr_w+bdr_s*2);
    s->scene.ui_viz_rw = (box_w+sbr_w-(bdr_s*2));
    s->scene.ui_viz_ro = 0;

    s->vision_connect_firstrun = false;

    s->alert_blinking_alpha = 1.0;
    s->alert_blinked = false;
  }

  zmq_pollitem_t polls[1] = {{0}};
  // Take an rgb image from visiond if there is one
  while(true) {
    assert(s->ipc_fd >= 0);
    polls[0].fd = s->ipc_fd;
    polls[0].events = ZMQ_POLLIN;
    #ifdef UI_60FPS
      // uses more CPU in both UI and surfaceflinger
      // 16% / 21%
      int ret = zmq_poll(polls, 1, 1);
    #else
      // 9% / 13% = a 14% savings
      int ret = zmq_poll(polls, 1, 1000);
    #endif
    if (ret < 0) {
      if (errno == EINTR || errno == EAGAIN) continue;

      LOGE("poll failed (%d - %d)", ret, errno);
      close(s->ipc_fd);
      s->ipc_fd = -1;
      s->vision_connected = false;
      return;
    } else if (ret == 0) {
      break;
    }
    // vision ipc event
    VisionPacket rp;
    err = vipc_recv(s->ipc_fd, &rp);
    if (err <= 0) {
      LOGW("vision disconnected");
      close(s->ipc_fd);
      s->ipc_fd = -1;
      s->vision_connected = false;
      return;
    }
    if (rp.type == VIPC_STREAM_ACQUIRE) {
      bool front = rp.d.stream_acq.type == VISION_STREAM_RGB_FRONT;
      int idx = rp.d.stream_acq.idx;

      int release_idx;
      if (front) {
        release_idx = s->cur_vision_front_idx;
      } else {
        release_idx = s->cur_vision_idx;
      }
      if (release_idx >= 0) {
        VisionPacket rep = {
          .type = VIPC_STREAM_RELEASE,
          .d = { .stream_rel = {
            .type = rp.d.stream_acq.type,
            .idx = release_idx,
          }},
        };
        vipc_send(s->ipc_fd, &rep);
      }

      if (front) {
        assert(idx < UI_BUF_COUNT);
        s->cur_vision_front_idx = idx;
      } else {
        assert(idx < UI_BUF_COUNT);
        s->cur_vision_idx = idx;
        // printf("v %d\n", ((uint8_t*)s->bufs[idx].addr)[0]);
      }
    } else {
      assert(false);
    }
    break;
  }
}

static int vision_subscribe(int fd, VisionPacket *rp, VisionStreamType type) {
  int err;
  LOGW("vision_subscribe type:%d", type);

  VisionPacket p1 = {
    .type = VIPC_STREAM_SUBSCRIBE,
    .d = { .stream_sub = { .type = type, .tbuffer = true, }, },
  };
  err = vipc_send(fd, &p1);
  if (err < 0) {
    close(fd);
    return 0;
  }

  do {
    err = vipc_recv(fd, rp);
    if (err <= 0) {
      close(fd);
      return 0;
    }

    // release what we aren't ready for yet
    if (rp->type == VIPC_STREAM_ACQUIRE) {
      VisionPacket rep = {
        .type = VIPC_STREAM_RELEASE,
        .d = { .stream_rel = {
          .type = rp->d.stream_acq.type,
          .idx = rp->d.stream_acq.idx,
        }},
      };
      vipc_send(fd, &rep);
    }
  } while (rp->type != VIPC_STREAM_BUFS || rp->d.stream_bufs.type != type);

  return 1;
}

static void* vision_connect_thread(void *args) {
  int err;
  set_thread_name("vision_connect");

  UIState *s = (UIState*)args;
  while (!do_exit) {
    usleep(100000);
    pthread_mutex_lock(&s->lock);
    bool connected = s->vision_connected;
    pthread_mutex_unlock(&s->lock);
    if (connected) continue;

    int fd = vipc_connect();
    if (fd < 0) continue;

    VisionPacket back_rp, front_rp;
    if (!vision_subscribe(fd, &back_rp, VISION_STREAM_RGB_BACK)) continue;
    if (!vision_subscribe(fd, &front_rp, VISION_STREAM_RGB_FRONT)) continue;

    pthread_mutex_lock(&s->lock);
    assert(!s->vision_connected);
    s->ipc_fd = fd;

    ui_init_vision(s,
                   back_rp.d.stream_bufs, back_rp.num_fds, back_rp.fds,
                   front_rp.d.stream_bufs, front_rp.num_fds, front_rp.fds);

    s->vision_connected = true;
    s->vision_seen = true;
    s->vision_connect_firstrun = true;

    // Drain sockets
    s->sm->drain();

    pthread_mutex_unlock(&s->lock);
  }
  return NULL;
}

#ifdef QCOM

#include <cutils/properties.h>
#include <hardware/sensors.h>
#include <utils/Timers.h>

static void* light_sensor_thread(void *args) {
  int err;
  set_thread_name("light_sensor");

  UIState *s = (UIState*)args;
  s->light_sensor = 0.0;

  struct sensors_poll_device_t* device;
  struct sensors_module_t* module;

  hw_get_module(SENSORS_HARDWARE_MODULE_ID, (hw_module_t const**)&module);
  sensors_open(&module->common, &device);

  // need to do this
  struct sensor_t const* list;
  int count = module->get_sensors_list(module, &list);

  int SENSOR_LIGHT = 7;

  err = device->activate(device, SENSOR_LIGHT, 0);
  if (err != 0) goto fail;
  err = device->activate(device, SENSOR_LIGHT, 1);
  if (err != 0) goto fail;

  device->setDelay(device, SENSOR_LIGHT, ms2ns(100));

  while (!do_exit) {
    static const size_t numEvents = 1;
    sensors_event_t buffer[numEvents];

    int n = device->poll(device, buffer, numEvents);
    if (n < 0) {
      LOG_100("light_sensor_poll failed: %d", n);
    }
    if (n > 0) {
      s->light_sensor = buffer[0].light;
    }
  }
  sensors_close(device);
  return NULL;

fail:
  LOGE("LIGHT SENSOR IS MISSING");
  s->light_sensor = 255;
  return NULL;
}

#endif

int is_leon() {
  #define MAXCHAR 1000
  FILE *fp;
  char str[MAXCHAR];
  const char* filename = "/proc/cmdline";

  fp = fopen(filename, "r");
  if (fp == NULL){
    printf("Could not open file %s",filename);
    return 0;
  }
  fgets(str, MAXCHAR, fp);
  fclose(fp);
  return strstr(str, "letv") != NULL;
}

int main(int argc, char* argv[]) {
  int err;
  setpriority(PRIO_PROCESS, 0, -14);

  zsys_handler_set(NULL);
  signal(SIGINT, (sighandler_t)set_do_exit);

  UIState uistate = {};
  UIState *s = &uistate;
  ui_init(s);
  enable_event_processing(true);

  pthread_t connect_thread_handle;
  err = pthread_create(&connect_thread_handle, NULL,
                       vision_connect_thread, s);
  assert(err == 0);

#ifdef QCOM
  pthread_t light_sensor_thread_handle;
  err = pthread_create(&light_sensor_thread_handle, NULL,
                       light_sensor_thread, s);
  assert(err == 0);
#endif

  TouchState touch = {0};
  touch_init(&touch);
  s->touch_fd = touch.fd;
  ui_sound_init();

  // light sensor scaling params
  const int LEON = is_leon();

  float brightness_b, brightness_m;
  int result = read_param_float(&brightness_b, "BRIGHTNESS_B", true);
  result += read_param_float(&brightness_m, "BRIGHTNESS_M", true);

  if(result != 0){
    brightness_b = LEON ? 10.0 : 5.0;
    brightness_m = LEON ? 2.6 : 1.3;
    write_param_float(brightness_b, "BRIGHTNESS_B", true);
    write_param_float(brightness_m, "BRIGHTNESS_M", true);
  }

  float smooth_brightness = brightness_b;

  const int MIN_VOLUME = LEON ? 12 : 9;
  const int MAX_VOLUME = LEON ? 15 : 12;

  set_volume(MIN_VOLUME);
  s->volume_timeout = 5 * UI_FREQ;
  int draws = 0;

  s->scene.satelliteCount = -1;
  s->started = false;
  s->vision_seen = false;

  while (!do_exit) {
    bool should_swap = false;
    if (!s->started) {
      // Delay a while to avoid 9% cpu usage while car is not started and user is keeping touching on the screen.
      // Don't hold the lock while sleeping, so that vision_connect_thread have chances to get the lock.
      usleep(30 * 1000);
    }
    pthread_mutex_lock(&s->lock);
    double u1 = millis_since_boot();

    // light sensor is only exposed on EONs
    float clipped_brightness = (s->light_sensor*brightness_m) + brightness_b;
    if (clipped_brightness > 512) clipped_brightness = 512;
    smooth_brightness = clipped_brightness * 0.01 + smooth_brightness * 0.99;
    if (smooth_brightness > 255) smooth_brightness = 255;
    set_brightness(s, (int)smooth_brightness);

    // resize vision for collapsing sidebar
    const bool hasSidebar = !s->scene.uilayout_sidebarcollapsed;
    s->scene.ui_viz_rx = hasSidebar ? box_x : (box_x - sbr_w + (bdr_s * 2));
    s->scene.ui_viz_rw = hasSidebar ? box_w : (box_w + sbr_w - (bdr_s * 2));
    s->scene.ui_viz_ro = hasSidebar ? -(sbr_w - 6 * bdr_s) : 0;

    // poll for touch events
    int touch_x = -1, touch_y = -1;
    int touched = touch_poll(&touch, &touch_x, &touch_y, 0);
    if (touched == 1) {
      set_awake(s, true);
      handle_sidebar_touch(s, touch_x, touch_y);
      handle_vision_touch(s, touch_x, touch_y);
    }

    if (!s->started) {
      // always process events offroad
      check_messages(s);
    } else {
      set_awake(s, true);
      // Car started, fetch a new rgb image from ipc
      if (s->vision_connected){
        ui_update(s);
      }

      check_messages(s);

      // Visiond process is just stopped, force a redraw to make screen blank again.
      if (!s->started) {
        s->scene.uilayout_sidebarcollapsed = false;
        update_offroad_layout_state(s);
        ui_draw(s);
        glFinish();
        should_swap = true;
      }
    }

    // manage wakefulness
    if (s->awake_timeout > 0) {
      s->awake_timeout--;
    } else {
      set_awake(s, false);
    }

    // manage hardware disconnect
    if (s->hardware_timeout > 0) {
      s->hardware_timeout--;
    } else {
      s->scene.hwType = cereal::HealthData::HwType::UNKNOWN;
    }

    // Don't waste resources on drawing in case screen is off
    if (s->awake) {
      ui_draw(s);
      glFinish();
      should_swap = true;
    }

    if (s->volume_timeout > 0) {
      s->volume_timeout--;
    } else {
      int volume = fmin(MAX_VOLUME, MIN_VOLUME + s->scene.v_ego / 5);  // up one notch every 5 m/s
      set_volume(volume);
      s->volume_timeout = 5 * UI_FREQ;
    }

    // If car is started and controlsState times out, display an alert
    if (s->controls_timeout > 0) {
      s->controls_timeout--;
    } else {
      if (s->started && s->controls_seen && s->scene.alert_text2 != "Controls Unresponsive") {
        LOGE("Controls unresponsive");
        s->scene.alert_size = cereal::ControlsState::AlertSize::FULL;
        update_status(s, STATUS_ALERT);

        s->scene.alert_text1 = "TAKE CONTROL IMMEDIATELY";
        s->scene.alert_text2 = "Controls Unresponsive";

        ui_draw_vision_alert(s, s->scene.alert_size, s->status, s->scene.alert_text1.c_str(), s->scene.alert_text2.c_str());

        s->alert_sound_timeout = 2 * UI_FREQ;
        s->alert_sound = cereal::CarControl::HUDControl::AudibleAlert::CHIME_WARNING_REPEAT;
        play_alert_sound(s->alert_sound);
      }

      s->alert_sound_timeout--;
      s->controls_seen = false;
    }

    // stop playing alert sound
    if ((!s->started || (s->started && s->alert_sound_timeout == 0)) &&
        s->alert_sound != cereal::CarControl::HUDControl::AudibleAlert::NONE) {
      stop_alert_sound(s->alert_sound);
      s->alert_sound = cereal::CarControl::HUDControl::AudibleAlert::NONE;
    }

    read_param_bool_timeout(&s->is_metric, "IsMetric", &s->is_metric_timeout);
    read_param_bool_timeout(&s->longitudinal_control, "LongitudinalControl", &s->longitudinal_control_timeout);
    read_param_bool_timeout(&s->limit_set_speed, "LimitSetSpeed", &s->limit_set_speed_timeout);
    read_param_float_timeout(&s->speed_lim_off, "SpeedLimitOffset", &s->limit_set_speed_timeout);
    int param_read = read_param_uint64_timeout(&s->last_athena_ping, "LastAthenaPingTime", &s->last_athena_ping_timeout);
    if (param_read != -1) { // Param was updated this loop
      if (param_read != 0) { // Failed to read param
        s->scene.athenaStatus = NET_DISCONNECTED;
      } else if (nanos_since_boot() - s->last_athena_ping < 70e9) {
        s->scene.athenaStatus = NET_CONNECTED;
      } else {
        s->scene.athenaStatus = NET_ERROR;
      }
    }
    update_offroad_layout_timeout(s, &s->offroad_layout_timeout);

    pthread_mutex_unlock(&s->lock);

    // the bg thread needs to be scheduled, so the main thread needs time without the lock
    // safe to do this outside the lock?
    if (should_swap) {
      double u2 = millis_since_boot();
      if (u2-u1 > 66) {
        // warn on sub 15fps
        LOGW("slow frame(%d) time: %.2f", draws, u2-u1);
      }
      draws++;
      framebuffer_swap(s->fb);
    }
  }

  set_awake(s, true);
  ui_sound_destroy();

  // wake up bg thread to exit
  pthread_mutex_lock(&s->lock);
  pthread_mutex_unlock(&s->lock);

#ifdef QCOM
  // join light_sensor_thread?
#endif

  err = pthread_join(connect_thread_handle, NULL);
  assert(err == 0);
  delete s->sm;
  delete s->pm;
  return 0;
}
