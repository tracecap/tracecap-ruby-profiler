require "mkmf"

create_makefile "tracecap_profiler/tracecap_profiler"

# patch in dtrace bits, since mkmf seems to be missing options to override this?

new_mf = []
File.open('Makefile', 'r') do |f|
  f.each_line do |line|
    line.gsub!(/^OBJS = /, 'OBJS = probes.o ') if RUBY_PLATFORM =~ /linux/
    line.gsub!(/^HDRS = /, 'HDRS = probes.h ')
    new_mf << line
  end
end
File.write('Makefile', new_mf.join())
