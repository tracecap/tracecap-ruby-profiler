require "rake/extensiontask"
require "bundler/gem_tasks"
task :default => :spec

Rake::ExtensionTask.new "tracecap_profiler" do |ext|
  ext.lib_dir = "lib/tracecap_profiler"
end
