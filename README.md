# tracecap-ruby-profiler

Exports Ruby VM (MRI) stack and object allocation count traces via USDT for tracecap ingestion. The design and implementation of the profiling code is heavily inspired by [stackprof](https://github.com/tmm1/stackprof).

## Installation

Add this line to your application's Gemfile:

```ruby
gem 'tracecap_profiler'
```

And then execute:

    $ bundle install

Or install it yourself as:

    $ gem install tracecap_profiler

## Usage

To enable tracepoints, use:
```ruby
TracecapProfiler::enable
```

Once this is enabled, `tracecap_profiler` will poll for the usage of its tracepoints and then begin emitting them as needed. There are 2 speeds available - standard at 99Hz and fast at 999Hz. The current Ruby and userspace C backtraces can be traced directly with `dtrace`:
```
$ sudo dtrace -n 'ruby-sample-std { trace(copyinstr(arg1)); ustack(); }'
$ sudo dtrace -n 'ruby-sample-fast { trace(copyinstr(arg1)); ustack(); }'
```

## Contributing

Bug reports and pull requests are welcome on GitHub at https://github.com/tracecap/tracecap-ruby-profiler.

## License

The gem is available as open source under the terms of the [MIT License](https://opensource.org/licenses/MIT).
