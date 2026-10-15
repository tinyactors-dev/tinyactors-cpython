# frozen_string_literal: true

# The wheel (docs/adr/0001, decisions 21 to 24): one abi3 wheel per native
# release target, built with the root's zig (Toolchain::Target.release)
# against that target's own CPython 3.13 headers, checked by its symbols,
# and zipped here, reproducibly. The release task (mise/tasks/release) and
# the wheel task (mise/tasks/wheel) both build through this file.
require 'base64'
require 'digest'
require 'fileutils'
require 'json'
require 'tmpdir'
require 'zlib'
require 'release'
require_relative 'cpython'

module CPython
  module Wheel
    # The headers the extension compiles against: python-build-standalone's
    # build of this release for the target's platform, installed by the
    # pinned uv without its executables. Any 3.13 is the same limited API;
    # pyconfig.h is the target's own (sizes and alignments differ between
    # macOS and Linux, and musl's is the one zig's musl headers match).
    PYTHON = '3.13.16'
    # The wheel's tags: abi3 from 3.13 (Py_LIMITED_API 0x030D0000).
    INTERPRETER = 'cp313'
    ABI = 'abi3'
    # Each native release target: its standalone Python and its platform
    # tags. A Linux extension links no C library (decision 10), so one file
    # serves glibc from 2.17 and musl from 1.1: the wheel carries both tags.
    # macOS 11.0 is the release targets' minimum (Toolchain::Target::MACOS_MIN).
    TARGETS = {
      'aarch64-macos' => { python: 'macos-aarch64-none', platforms: %w[macosx_11_0_arm64] },
      'x86_64-macos' => { python: 'macos-x86_64-none', platforms: %w[macosx_11_0_x86_64] },
      'x86_64-linux' => { python: 'linux-x86_64-musl', platforms: %w[manylinux_2_17_x86_64 musllinux_1_1_x86_64] },
      'aarch64-linux' => { python: 'linux-aarch64-musl', platforms: %w[manylinux_2_17_aarch64 musllinux_1_1_aarch64] }
    }.freeze
    # What the extension exports: CPython's import finds it by this name.
    EXPORTS = %w[PyInit_tinyactors].freeze
    # The C library functions it calls, which the interpreter's process has:
    # each has the same name in glibc and musl, so a Linux extension that
    # links no C library binds to either. The math functions are in glibc's
    # libm, which the interpreter links, so they are in the process the
    # extension is loaded into. On macOS libSystem has them, malloc_size in
    # place of malloc_usable_size; Darwin's clang calls bzero and the
    # fortified __memcpy_chk and __memmove_chk, and dyld binds lazily. A new
    # one fails the build until it is checked and listed (ADR 0001,
    # "Consequences").
    LIBC = %w[abort acos acosh asin asinh atan atan2 atanh bcmp calloc cbrt ceil clock_gettime cos cosh exp expm1
              fclose floor fmod fopen fprintf fputc fread free frexp fseek ftell fwrite gettimeofday hypot
              localtime_r log log10 log1p log2 malloc malloc_usable_size memchr memcmp memcpy memmove memset pow
              printf qsort realloc round scalbn sin sinh snprintf sscanf strchr strcmp strlen strncmp strrchr
              strstr strtod tan tanh trunc vsnprintf].freeze
    DARWIN = %w[bzero __bzero __memcpy_chk __memmove_chk malloc_size dyld_stub_binder].freeze
    # The C API: the limited API's functions and data (Py_LIMITED_API keeps
    # the headers to it), which the interpreter provides.
    PYTHON_SYMBOL = /\A_?Py/
    # The flags the extension's C compiles with in a release: the
    # development build's without its debug information and optimization,
    # which the release target's flags set (-O2).
    FLAGS = (CPython::FLAGS - %w[-g -O1]).freeze

    module_function

    def fail!(message)
      abort "wheel: #{message}"
    end

    def target_names = TARGETS.keys

    def platforms(name) = TARGETS.fetch(name)[:platforms]

    # The include directory of the target's standalone Python, installed
    # into build/pythons by the pinned uv when it is missing.
    def include_directory(name)
      key = "cpython-#{PYTHON}-#{TARGETS.fetch(name)[:python]}"
      root = BUILD.join('pythons')
      include = root.join(key, 'include', "python#{PYTHON[/\A\d+\.\d+/]}")
      return include if include.join('Python.h').file?

      CPython.run(CPython.pinned('uv'), 'python', 'install', '--quiet', '--no-bin', key,
                  env: { 'UV_PYTHON_INSTALL_DIR' => root.to_s })
      fail!("#{key} has no Python.h at #{include}") unless include.join('Python.h').file?
      include
    end

    # The extension for one release target, in build/release/<target>/:
    # core's amalgamation and src/ compiled with zig for the target and
    # linked as a module the interpreter loads (Target#plugin). Answers its
    # path.
    def build(name)
      target = Toolchain::Target.release(name)
      dir = BUILD.join('release', name)
      core = CORE_SOURCES.map do |file, flags|
        CPython.compile(target, CORE_BUILD.join(file), dir.join('objects/core', file.sub(/\.c\z/, '.o')),
                        [*flags, "-I#{CORE_BUILD}"])
      end
      includes = ["-I#{CORE_BUILD}", "-I#{ROOT.join('src')}", "-I#{CPython.support_include}",
                  "-I#{include_directory(name)}"]
      objects = Dir[ROOT.join(SOURCES).to_s].sort.map do |source|
        relative = Pathname.new(source).relative_path_from(ROOT.join('src')).sub_ext('.o')
        CPython.compile(target, source, dir.join('objects/src', relative),
                        [*FLAGS, *includes, '-DPy_LIMITED_API=0x030D0000'])
      end
      extension = dir.join('tinyactors.abi3.so')
      target.plugin([*objects, *core].map(&:to_s), into: extension.to_s)
      verify!(target, extension)
      extension
    end

    # The extension is for its target, loads no shared library (libSystem
    # aside: the C library and the interpreter are the process's, which is
    # what lets one Linux file claim manylinux and musllinux), exports only
    # its init function, and leaves undefined only the C API and the listed
    # C library functions.
    def verify!(target, extension)
      binary = Toolchain::Binary.new(extension)
      binary.must_be_for!(target)
      binary.must_load_only!([])
      binary.must_export_exactly!(EXPORTS)
      libc = target.format.links_libc ? LIBC + DARWIN : LIBC
      binary.must_leave_undefined_only!(binary.undefined.to_a.grep(PYTHON_SYMBOL) + libc)
    rescue Toolchain::Binary::Mismatch => e
      fail!("#{extension.relative_path_from(ROOT)}: #{e.message}")
    end

    # pyproject.toml's [project] table, read by the pinned python's tomllib
    # (Ruby has no TOML parser).
    def project
      script = 'import json, sys, tomllib; print(json.dumps(tomllib.load(open(sys.argv[1], "rb"))["project"]))'
      JSON.parse(IO.popen([CPython.pinned('python'), '-c', script, ROOT.join('pyproject.toml').to_s], &:read))
    end

    # The wheel's file name (PEP 427, with PEP 425's compressed tag set).
    def file_name(version, name)
      "tinyactors-#{version}-#{INTERPRETER}-#{ABI}-#{platforms(name).join('.')}.whl"
    end

    # The core metadata (Metadata-Version 2.4, PEP 639's license fields),
    # with the README as the long description: its relative links point at
    # the repository at `commit`, since PyPI shows it on its own.
    def metadata(project, version, commit)
      urls = project.fetch('urls', {})
      repository = urls.fetch('Repository')
      readme = ROOT.join(project.fetch('readme')).read.gsub(/\]\((?![a-z]+:|#)([^)]+)\)/) do
        "](#{repository}/blob/#{commit}/tinyactors-cpython/#{Regexp.last_match(1)})"
      end
      lines = ['Metadata-Version: 2.4', "Name: #{project.fetch('name')}", "Version: #{version}",
               "Summary: #{project.fetch('description')}",
               "Keywords: #{project.fetch('keywords', []).join(',')}",
               *project.fetch('classifiers', []).map { |classifier| "Classifier: #{classifier}" },
               *urls.map { |label, url| "Project-URL: #{label}, #{url}" },
               "License-Expression: #{project.fetch('license')}", 'License-File: LICENSE',
               "Requires-Python: #{project.fetch('requires-python')}",
               'Description-Content-Type: text/markdown']
      "#{lines.join("\n")}\n\n#{readme}"
    end

    def wheel_file(name)
      tags = platforms(name).map { |platform| "Tag: #{INTERPRETER}-#{ABI}-#{platform}" }
      "#{['Wheel-Version: 1.0', 'Generator: tinyactors-cpython (mise run wheel)', 'Root-Is-Purelib: false',
          *tags].join("\n")}\n"
    end

    # Packs the extension for `name` as a wheel in `out`; answers its path.
    # The entries are in a fixed order with the time of `epoch` (seconds), so
    # the same commit gives the same bytes.
    def pack(name, extension, version:, out:, commit:, epoch:)
      project = self.project
      dist_info = "tinyactors-#{version}.dist-info"
      files = [['tinyactors.abi3.so', extension.binread, 0o755],
               ["#{dist_info}/METADATA", metadata(project, version, commit), 0o644],
               ["#{dist_info}/WHEEL", wheel_file(name), 0o644],
               ["#{dist_info}/licenses/LICENSE", ROOT.join('../LICENSE').read, 0o644]]
      record = files.map { |path, data, _| "#{path},sha256=#{digest(data)},#{data.bytesize}" }
      files << ["#{dist_info}/RECORD", "#{[*record, "#{dist_info}/RECORD,,"].join("\n")}\n", 0o644]
      FileUtils.mkdir_p(out)
      path = Pathname.new(out).join(file_name(version, name))
      path.binwrite(Zip.archive(files, epoch))
      path
    end

    # The development version of HEAD in PEP 440's form (decision 23):
    # Release.dev_version's <next minor>-dev.<date>.<N>.g<sha> becomes
    # <next minor>.dev<date><N, five digits>, which sorts the same way on
    # linear main; PyPI takes no local version, so the sha is left out.
    # Once a release is on PyPI, pip and uv pick a dev release only when
    # asked (--pre); while there is none, they pick the newest dev release.
    def dev_version(prefix)
      semver = Release.dev_version(prefix)
      match = /\A(\d+\.\d+\.\d+)-dev\.(\d{8})\.(\d+)\.g\h+\z/.match(semver) or fail!("cannot read #{semver}")
      # six digits would sort a later commit of the same day before an earlier one
      fail!("#{match[3]} commits since the last release do not fit a dev number: release first") if match[3].to_i >= 100_000
      format('%<base>s.dev%<date>s%<count>05d', base: match[1], date: match[2], count: match[3].to_i)
    end

    # The wheels of `names` for `version` in `out`: built, checked, packed.
    # Answers {target name => wheel path}.
    def build_all(names, version:, out:)
      commit = Release.git('rev-parse', 'HEAD').to_s.strip
      epoch = Release.git('show', '-s', '--format=%ct', 'HEAD').to_i
      names.to_h do |name|
        wheel = pack(name, build(name), version: version, out: out, commit: commit, epoch: epoch)
        puts "wheel: #{wheel.relative_path_from(ROOT)} (#{wheel.size / 1024} KiB)"
        [name, wheel]
      end
    end

    # The interpreters this machine's wheel is installed into: the floor the
    # abi3 tag claims, and a later one, which loads the same file.
    SMOKE_PYTHONS = %w[3.13 3.14].freeze

    # This machine's wheel, installed into a fresh environment for each of
    # SMOKE_PYTHONS by the pinned uv: the smoke test runs there, isolated
    # from the build, and on the first interpreter the binding's own tests
    # run against the installed wheel too (tests/ and the adapter import
    # tinyactors, which only the environment has).
    def smoke_here!(wheel, tests: true)
      uv = CPython.pinned('uv')
      Dir.mktmpdir('tinyactors-wheel') do |dir|
        SMOKE_PYTHONS.each_with_index do |request, index|
          venv = File.join(dir, request)
          # installed first: with only the free-threaded build of a release
          # on the machine, uv would pick it, and abi3 cannot load there
          CPython.run(uv, 'python', 'install', '--quiet', '--no-bin', request)
          CPython.run(uv, 'venv', '--quiet', '--python', request, venv)
          python = File.join(venv, 'bin', 'python')
          CPython.run(uv, 'pip', 'install', '--quiet', '--python', python, '--no-deps', wheel.to_s)
          CPython.run(python, '-I', ROOT.join('tools/smoke.py').to_s, env: BIND_NOW)
          next unless tests && index.zero?

          puts "wheel: tests/ against the installed wheel, Python #{request}"
          CPython.run(python, '-m', 'unittest', 'discover', '-q', '-s', 'tests', '-t', 'tests', env: BIND_NOW)
        end
      end
    end

    # The smoke tests resolve every symbol when the extension is loaded,
    # not when a function is first called: a C library function the
    # interpreter's process lacks (glibc's libm, if an interpreter did not
    # load it) fails the import, not a later run. macOS's dyld ignores it.
    BIND_NOW = { 'LD_BIND_NOW' => '1' }.freeze

    # The images a Linux wheel is smoke-tested in, one per C library.
    CONTAINERS = { 'glibc' => 'python:3.13-slim', 'musl' => 'python:3.13-alpine' }.freeze

    # Each Linux wheel installed with pip and smoke-tested in a container of
    # its architecture (under qemu for the other one), for glibc and musl.
    def smoke_in_containers!(wheels)
      wheels.each do |name, wheel|
        platform = Toolchain::Target.release(name).container_platform or next
        CONTAINERS.each do |libc, image|
          script = 'pip install --quiet --no-index --no-deps /w/"$0" && python -I /t/smoke.py'
          CPython.run('docker', 'run', '--rm', '--platform', platform,
                      '-e', 'PIP_ROOT_USER_ACTION=ignore', '-e', 'PIP_DISABLE_PIP_VERSION_CHECK=1', '-e', 'LD_BIND_NOW=1',
                      '-v', "#{wheel.dirname}:/w:ro", '-v', "#{ROOT.join('tools')}:/t:ro",
                      image, 'sh', '-c', script, wheel.basename.to_s)
          puts "wheel: #{name} on #{libc} (#{image}, #{platform}): ok"
        end
      end
    end

    # RECORD's hash: urlsafe base64 of the SHA-256, without padding.
    def digest(data) = Base64.urlsafe_encode64(Digest::SHA256.digest(data), padding: false)

    # A zip archive of [path, data, mode] entries, deflated, every entry
    # stamped with `epoch`. Only what a wheel needs: no zip64, no
    # directories, no comments.
    module Zip
      module_function

      def archive(files, epoch)
        time, date = dos_time(epoch)
        body = String.new(encoding: Encoding::BINARY)
        directory = String.new(encoding: Encoding::BINARY)
        files.each do |path, data, mode|
          data = data.b
          compressed = deflate(data)
          crc = Zlib.crc32(data)
          name = path.b
          offset = body.bytesize
          fields = [20, 0x0800, 8, time, date, crc, compressed.bytesize, data.bytesize, name.bytesize, 0]
          body << [0x04034b50, *fields].pack('VvvvvvVVVvv') << name << compressed
          directory << [0x02014b50, (3 << 8) | 20, *fields, 0, 0, 0, (0o100000 | mode) << 16, offset]
                       .pack('VvvvvvvVVVvvvvvVV') << name
        end
        body << directory
        body << [0x06054b50, 0, 0, files.size, files.size, directory.bytesize, body.bytesize - directory.bytesize, 0]
                .pack('VvvvvVVv')
      end

      def deflate(data)
        stream = Zlib::Deflate.new(Zlib::BEST_COMPRESSION, -Zlib::MAX_WBITS)
        compressed = stream.deflate(data, Zlib::FINISH)
        stream.close
        compressed
      end

      # MS-DOS time and date of `epoch` in UTC, from 1980 (zip's first year).
      def dos_time(epoch)
        t = Time.at([epoch, 315_532_800].max).utc
        [(t.hour << 11) | (t.min << 5) | (t.sec / 2), ((t.year - 1980) << 9) | (t.month << 5) | t.day]
      end
    end
  end
end
