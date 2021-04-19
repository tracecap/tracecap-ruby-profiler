provider tracecap_ruby_profiler {
  probe ruby__sample__std(struct ruby_sample *, uint64_t, const char *);
  probe ruby__sample__fast(struct ruby_sample *, uint64_t, const char *);
};
