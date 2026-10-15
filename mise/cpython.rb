# frozen_string_literal: true

# What tinyactors-cpython's tasks share: where things are, how a task runs
# a command, and the interpreters this project pins. The extension's build
# belongs here too (Toolchain::Target.host for the tests, Target.release
# for the wheel; mise/lib/toolchain.rb).
require 'pathname'
require 'cache'
require 'toolchain'

module CPython
  ROOT = Pathname.new(__dir__).join('..').expand_path
  BUILD = ROOT.join('build')
  # Core's amalgamation, built from tinyactors-core's sources into this
  # project's own build/ (AGENTS.md, "Tasks").
  CORE_BUILD = BUILD.join('core')
  # The suite's adapter, `python -m conform` (tinyactors-test/docs/driver.md).
  ADAPTER = ROOT.join('conform')
  # The mapping table (tinyactors-core/docs/bindings.md, section 2).
  MAPPING = ROOT.join('docs/api-map.md')
  # The suite's set `mise run test` runs (`--milestone`, a set of
  # tinyactors-test/suites/milestones.txt), named here and nowhere else.
  # Which set the binding is held to now is in ROADMAP.md.
  MILESTONE = 'M4'
  # What of that set `mise run test` runs now: the suites (`--suite`) and
  # the datamodels the adapter passes so far. It grows with the plan
  # (ROADMAP.md, "Plan and progress"): M4's protocol vectors, the W3C
  # suite (M2: on the native datamodel less the tests
  # tinyactors-test/suites/w3c/skip-python.txt names) and every scenario
  # (invoke, hibernate, migrate, snapshot) pass on both datamodels.
  GATE = { suites: %w[protocol w3c scenarios], datamodels: %w[native quickjs] }.freeze

  # The extension's C (AGENTS.md, "Layout"), and the flags every file of it
  # compiles with: all of its warnings are errors, and nothing but
  # PyInit_tinyactors is exported (PyMODINIT_FUNC makes it visible).
  SOURCES = 'src/**/*.c'
  FLAGS = %w[-std=c11 -Wall -Wextra -Werror -Wno-missing-field-initializers -fvisibility=hidden -g -O1].freeze
  # Core's amalgamation (tinyactors-core/mise/core.rb, LIBRARIES): the
  # library, the runtime with its hosted platform file (the compiler and
  # the C library's allocator the compiler uses), and the ECMAScript
  # datamodel, with the flags core compiles quickjs with (QUICKJS_FLAGS).
  CORE_SOURCES = {
    'tinyactors.c' => %w[-std=c11],
    'tinyactors_runtime.c' => %w[-std=c11],
    'tinyactors_runtime_hosted.c' => %w[-std=c11],
    'tinyactors_datamodel_ecmascript.c' => %w[-std=gnu11 -w -fno-strict-aliasing -D_GNU_SOURCE -D__STDC_NO_ATOMICS__=1]
  }.freeze

  # An interpreter the extension is built for: the abi3 build on the pinned
  # 3.13, which the wheel ships, and the same sources again without
  # Py_LIMITED_API for the free-threaded interpreter, which only the tests
  # use (AGENTS.md, "C rules").
  Build = Struct.new(:name, :python, :defines, :suffix, keyword_init: true) do
    def directory = BUILD.join(name)
    def extension = directory.join("tinyactors#{suffix}")
  end

  module_function

  # Whether there is an extension to build: src/ has C files. Without
  # them the build stops at core's amalgamation and the tests have no
  # module to import (README.md: being rewritten).
  def extension? = !Dir[ROOT.join(SOURCES).to_s].empty?

  # Whether tests/ holds tests of the extension.
  def tests? = !Dir[ROOT.join('tests/**/test_*.py').to_s].empty?

  # Runs a command in the project's directory; a failure ends the task with
  # its status.
  def run(*command, env: {})
    pid = Process.spawn(env, *command, chdir: ROOT.to_s)
    _pid, status = Process.wait2(pid)
    exit(status.exitstatus || 1) unless status.success?
  end

  # The path of a program this project's mise.toml pins (python, uv). A
  # command another project's task runs gets it by path: that project's
  # pins come first on its PATH (tinyactors-test pins its own python).
  def pinned(program)
    path = IO.popen(['mise', 'which', program], chdir: ROOT.to_s, err: File::NULL, &:read).strip
    abort "#{program} is not installed: run `mise install` in #{ROOT}" if path.empty?
    path
  end

  # The two builds of the extension (Build).
  def builds
    abi3 = pinned('python')
    free_threaded = free_threaded_python
    [Build.new(name: 'abi3', python: abi3, defines: %w[Py_LIMITED_API=0x030D0000], suffix: '.abi3.so'),
     Build.new(name: 'free-threaded', python: free_threaded, defines: [],
               suffix: config_var(free_threaded, 'EXT_SUFFIX'))]
  end

  # A sysconfig value of an interpreter.
  def config_var(python, name)
    IO.popen([python, '-c', "import sysconfig; print(sysconfig.get_config_var(#{name.inspect}))"], &:read).strip
  end

  # Where an interpreter's Python.h is.
  def include_directory(python)
    IO.popen([python, '-c', 'import sysconfig; print(sysconfig.get_paths()["include"])'], &:read).strip
  end

  # Compiles one C file with the content cache (mise/lib/cache.rb); a
  # failure ends the task with the compiler's output.
  def compile(target, source, object, flags)
    command = target.compile_command(*target.format.position_independent, *flags)
    outcome, output = Cache.compile(command, source, object)
    return object unless outcome == :failed

    warn output
    abort "build: #{Pathname.new(source).relative_path_from(ROOT)} does not compile"
  end

  # Core's amalgamation as objects, the same for both builds: they hold no
  # Python.
  def core_objects(target)
    CORE_SOURCES.map do |file, flags|
      compile(target, CORE_BUILD.join(file), BUILD.join('objects/core', file.sub(/\.c\z/, '.o')),
              [*flags, '-fvisibility=hidden', '-g', '-O2', "-I#{CORE_BUILD}"])
    end
  end

  # The extension for one interpreter: src/ against its Python.h, linked
  # with core's objects into a module CPython loads (Target#plugin: the
  # interpreter provides the C API's symbols).
  def build_extension(target, build, core)
    generated = support_include
    includes = ["-I#{CORE_BUILD}", "-I#{ROOT.join('src')}", "-I#{generated}", "-I#{include_directory(build.python)}"]
    objects = Dir[ROOT.join(SOURCES).to_s].sort.map do |source|
      relative = Pathname.new(source).relative_path_from(ROOT.join('src')).sub_ext('.o')
      compile(target, source, build.directory.join('objects', relative),
              [*FLAGS, *includes, *build.defines.map { |define| "-D#{define}" }])
    end
    target.plugin([*objects, *core].map(&:to_s), into: build.extension.to_s)
    build.extension
  end

  # src/datamodel/support.py, the "python" datamodel's rewrite and the
  # exceptions, as one C string literal (build/generated/support.inc) that
  # src/datamodel/expressions.c includes: the module runs it once per
  # interpreter. It is no file of the wheel. Rewritten only when it changes,
  # so the compile cache (its -MMD dependencies) sees the change.
  SUPPORT = 'src/datamodel/support.py'

  def support_include
    directory = BUILD.join('generated')
    directory.mkpath
    literal = ROOT.join(SUPPORT).read.each_line.map do |line|
      escaped = line.chomp.gsub('\\') { '\\\\' }.gsub('"') { '\\"' }
      "\"#{escaped}\\n\"\n"
    end.join
    include = directory.join('support.inc')
    include.write(literal) unless include.file? && include.read == literal
    directory
  end

  # A stress tool on both datamodels against the abi3 build: N actors, at
  # most R resident, the rest hibernated into SQLite; tools/stress.py
  # (README.md, "Hibernation"), tools/migration.py (README.md,
  # "Migration": all of them move to a new version, live) or
  # tools/snapshot.py (README.md, "Snapshots": a deploy rehearsed on a
  # copy of a live host), or tools/services.py (README.md, "Invoked services": every
  # actor with a service, no store, so `resident` is not passed). A failed
  # check ends the task.
  def stress(actors:, events:, resident: nil, tool: 'stress')
    %w[python ecmascript].each do |datamodel|
      puts "#{tool}: #{datamodel}, #{actors} actors, #{resident || 'all'} resident, #{events} events"
      run(pinned('python'), "tools/#{tool}.py", '--datamodel', datamodel, '--actors', actors.to_s,
          *(resident ? ['--resident', resident.to_s] : []), '--events', events.to_s,
          env: { 'PYTHONPATH' => BUILD.join('abi3').to_s })
    end
  end

  # The free-threaded interpreter of mise.toml's TA_PYTHON_FREE_THREADED,
  # installed by the pinned uv when it is missing.
  def free_threaded_python
    request = ENV.fetch('TA_PYTHON_FREE_THREADED')
    uv = pinned('uv')
    run(uv, 'python', 'install', '--quiet', request)
    path = IO.popen([uv, 'python', 'find', '--managed-python', request], chdir: ROOT.to_s, &:read).strip
    abort "uv found no interpreter for #{request}" if path.empty?
    path
  end
end
