provider tracecap_ruby {
  probe ruby__sample__std(struct ruby_sample *, const char *);
  probe ruby__sample__fast(struct ruby_sample *, const char *);
};
