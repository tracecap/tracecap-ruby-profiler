#include <sys/time.h>
#include <signal.h>
#include <pthread.h>

#include <ruby/ruby.h>
#include <ruby/debug.h>

#include "ruby_sample.h"
#include "probes.h"

#define BUF_SIZE 2048

enum {
  TRACECAP_MODE_DISABLED = 0,
  TRACECAP_MODE_ENABLED_WATCHING = 1,
  TRACECAP_MODE_ENABLED_STANDARD = 2,
  TRACECAP_MODE_ENABLED_FAST = 3,
};

#define TRACECAP_INTERVAL_WATCHING_US (1000000 / 9) // 9Hz
#define TRACECAP_INTERVAL_STANDARD_US (1000000 / 99) // 99Hz
#define TRACECAP_INTERVAL_FAST_US (1000000 / 990) // 990Hz

static struct {
  int mode;

  VALUE stack_written_frames[BUF_SIZE];
  int stack_written_lines[BUF_SIZE];
  char *stack_written_offsets[BUF_SIZE];
  int stack_last_depth;
  char stack[81920];

  VALUE frames_buffer[BUF_SIZE];
  int lines_buffer[BUF_SIZE];
} _tracecap_data;

static void profiler_switch_mode(int new_mode);

static inline uint64_t tracecap_update_stack()
{
  int num;
  int stack_left = sizeof(_tracecap_data.stack);

  // read in the latest set of frames/lines
  num = rb_profile_frames(0, sizeof(_tracecap_data.frames_buffer) / sizeof(VALUE), _tracecap_data.frames_buffer, _tracecap_data.lines_buffer);

  // work out how many are the same as previously calculated
  int same = 0;
  char *stack_curr = _tracecap_data.stack;
  for (int i = 0; i < num && i < _tracecap_data.stack_last_depth; i++) {
    if (_tracecap_data.frames_buffer[num - 1 - i] == _tracecap_data.stack_written_frames[i] && _tracecap_data.lines_buffer[num - 1 - i] == _tracecap_data.stack_written_lines[i])
      same++;
    else
      break;
  }

  if (same > 0) {
    stack_curr = _tracecap_data.stack_written_offsets[same - 1];
    stack_left = (int)(_tracecap_data.stack + sizeof(_tracecap_data.stack) - stack_curr);
  }

  int i;
  for (i = same; i < num; i++) {
    VALUE frame = _tracecap_data.frames_buffer[num - 1 - i];
    int line = _tracecap_data.lines_buffer[num - 1 - i];

    _tracecap_data.stack_written_frames[i] = frame;
    _tracecap_data.stack_written_lines[i] = line;

    VALUE name = rb_profile_frame_full_label(frame);
    VALUE file = rb_profile_frame_path(frame);
    char *file_str = StringValueCStr(file);
    char maybe_package[128] = {0};
    char *gems_slash = strstr(file_str, "/gems/");
    char *next = gems_slash;
    
    // find the last occurance of /gems/
    while (next != NULL && *next != 0) {
      next = strstr(next + 1, "/gems/");
      if (next != NULL) {
        gems_slash = next;
      }
    }

    if (gems_slash != NULL) {
      char *gem_name_start = gems_slash + strlen("/gems/");
      char *inside_gem = strchr(gem_name_start, '/');
      if (inside_gem != NULL) {
        file_str = inside_gem + 1; // skip over '/'
        snprintf(maybe_package, sizeof(maybe_package), "%.*s:", (int)(inside_gem - gem_name_start), gem_name_start);
      }
    }

    if (stack_left > 0) {
      int n = snprintf(stack_curr, stack_left, "%s%s:%d:%s\n", maybe_package, file_str, line, StringValueCStr(name));
      stack_left -= n;
      stack_curr += n;
    } else {
      break;
    }

    _tracecap_data.stack_written_offsets[i] = stack_curr;
  }

  *stack_curr = '\0';
  _tracecap_data.stack_last_depth = i;

  return (uint64_t)(stack_curr - _tracecap_data.stack);
}

static inline void tracecap_handle_tracing()
{
  static VALUE countedObjects = Qnil;
  int expected_mode = TRACECAP_MODE_ENABLED_WATCHING;
  static int traces = 0;

  if (TRACECAP_RUBY_PROFILER_RUBY_SAMPLE_STD_ENABLED() || TRACECAP_RUBY_PROFILER_RUBY_SAMPLE_FAST_ENABLED()) {
    struct ruby_sample sample = {};

    uint64_t stack_len = tracecap_update_stack();

    if (countedObjects == Qnil) {
      countedObjects = rb_hash_new();
    }

    sample.object_space.total = rb_gc_stat(ID2SYM(rb_intern("heap_available_slots")));
    sample.object_space.free = rb_gc_stat(ID2SYM(rb_intern("heap_free_slots")));

    if (TRACECAP_RUBY_PROFILER_RUBY_SAMPLE_STD_ENABLED()) {
      expected_mode = TRACECAP_MODE_ENABLED_STANDARD;

      // if we're configured for standard speed, output every time.
      // if we're configured for fast speed, output every 10, to match std speed.
      if (_tracecap_data.mode == TRACECAP_MODE_ENABLED_STANDARD ||
          (_tracecap_data.mode == TRACECAP_MODE_ENABLED_FAST && (traces % 10) == 0)) {
        TRACECAP_RUBY_PROFILER_RUBY_SAMPLE_STD(&sample, stack_len, _tracecap_data.stack);
      }
    }

    if (TRACECAP_RUBY_PROFILER_RUBY_SAMPLE_FAST_ENABLED()) {
      expected_mode = TRACECAP_MODE_ENABLED_FAST;
      TRACECAP_RUBY_PROFILER_RUBY_SAMPLE_FAST(&sample, stack_len, _tracecap_data.stack);
    }
  }

  // if we are hooked in a different mode to the enabled one we're on, switch to it.
  if (_tracecap_data.mode != TRACECAP_MODE_DISABLED && expected_mode != _tracecap_data.mode)
    profiler_switch_mode(expected_mode);

  traces++;
}

static void tracecap_job_handler(void *data)
{
  static int in_signal_handler = 0;
  if (_tracecap_data.mode == TRACECAP_MODE_DISABLED) return;
  if (in_signal_handler) return;

  in_signal_handler++;
  tracecap_handle_tracing();
  in_signal_handler--;
}

static void tracecap_signal_handler(int sig, siginfo_t *sinfo, void *ucontext)
{
  // we won't emit traces during GC, it's possible to trace GC itself
  if (rb_during_gc()) return;

  rb_postponed_job_register_one(0, tracecap_job_handler, (void*)0);
}

static void profiler_switch_mode(int new_mode)
{
  struct sigaction sa;
  struct itimerval timer;

  if (_tracecap_data.mode == new_mode) return;

  sa.sa_sigaction = tracecap_signal_handler;
	sa.sa_flags = SA_RESTART | SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGALRM, &sa, NULL);

  if (new_mode == TRACECAP_MODE_DISABLED) {
    memset(&timer, 0, sizeof(timer));
    setitimer(ITIMER_REAL, &timer, 0);
  } else {
    timer.it_interval.tv_sec = 0;
    if (new_mode == TRACECAP_MODE_ENABLED_STANDARD)
      timer.it_interval.tv_usec = TRACECAP_INTERVAL_STANDARD_US;
    else if (new_mode == TRACECAP_MODE_ENABLED_FAST)
      timer.it_interval.tv_usec = TRACECAP_INTERVAL_FAST_US;
    else
      timer.it_interval.tv_usec = TRACECAP_INTERVAL_WATCHING_US;
    timer.it_value = timer.it_interval;
    setitimer(ITIMER_REAL, &timer, 0);
  }

  _tracecap_data.mode = new_mode;
}

static VALUE profiler_enable(VALUE self)
{
  if (_tracecap_data.mode == TRACECAP_MODE_DISABLED) {
    profiler_switch_mode(TRACECAP_MODE_ENABLED_WATCHING);
  }
  return Qtrue;
}

static VALUE profiler_disable(VALUE self)
{
  if (_tracecap_data.mode != TRACECAP_MODE_DISABLED) {
    profiler_switch_mode(TRACECAP_MODE_DISABLED);
  }
  return Qtrue;
}

static void profiler_atfork_prepare(void)
{
  struct itimerval timer;
  if (_tracecap_data.mode != TRACECAP_MODE_DISABLED) {
    memset(&timer, 0, sizeof(timer));
    setitimer(ITIMER_REAL, &timer, 0);
  }
}

static void profiler_atfork_parent(void)
{
  if (_tracecap_data.mode != TRACECAP_MODE_DISABLED) {
    int new_mode = _tracecap_data.mode;
    _tracecap_data.mode = TRACECAP_MODE_DISABLED;
    profiler_switch_mode(new_mode);
  }
}

static void profiler_atfork_child(void)
{
  profiler_switch_mode(TRACECAP_MODE_DISABLED);
}

void
Init_tracecap_profiler(void) {
  VALUE tracecap_profiler;

  _tracecap_data.mode = TRACECAP_MODE_DISABLED;
  _tracecap_data.stack_last_depth = 0;

  tracecap_profiler = rb_const_get(rb_cObject, rb_intern("TracecapProfiler"));
  rb_define_singleton_method(tracecap_profiler, "enable", profiler_enable, 0);
  rb_define_singleton_method(tracecap_profiler, "disable", profiler_disable, 0);

  pthread_atfork(profiler_atfork_prepare, profiler_atfork_parent, profiler_atfork_child);
}
