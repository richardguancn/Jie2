import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('dependencies', Path(__file__).parents[1] / 'scripts/windows-dependencies.py')
deps = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deps)

class DependenciesTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.package = Path(self.temp.name) / 'package'
        self.runtime = Path(self.temp.name) / 'runtime'
        self.package.mkdir(); self.runtime.mkdir()
        (self.package / 'jie2.exe').touch()
        (self.package / 'platforms').mkdir()
        (self.package / 'platforms/qwindows.dll').touch()
        for name in ['Qt6Core.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll', 'libfreetype-6.dll', 'libpng16-16.dll', 'zlib1.dll']:
            (self.runtime / name).touch()
        self.graph = {
            'jie2.exe': ['KERNEL32.dll', 'Qt6Core.dll'],
            'qwindows.dll': ['libfreetype-6.dll', 'USER32.dll'],
            'Qt6Core.dll': ['libstdc++-6.dll', 'api-ms-win-crt-runtime-l1-1-0.dll'],
            'libstdc++-6.dll': ['libwinpthread-1.dll'],
            'libwinpthread-1.dll': ['ucrtbase.dll'],
            'libfreetype-6.dll': ['libpng16-16.dll', 'zlib1.dll'],
            'libpng16-16.dll': ['ZLIB1.DLL'],
            'zlib1.dll': ['KERNEL32.dll'],
        }
    def scan(self, path): return self.graph[path.name]
    def test_transitive_and_plugin_dependencies(self):
        copied, count = deps.collect(self.package, [self.runtime], self.scan)
        self.assertEqual(len(copied), 6)
        self.assertEqual(count, 8)
        self.assertEqual(deps.collect(self.package, [], self.scan, True), ([], 8))
    def test_missing_transitive_dependency_stops_packaging(self):
        (self.runtime / 'libpng16-16.dll').unlink()
        with self.assertRaisesRegex(RuntimeError, 'libfreetype-6.dll -> libpng16-16.dll'):
            deps.collect(self.package, [self.runtime], self.scan)
    def test_audit_does_not_use_developer_runtime(self):
        with self.assertRaisesRegex(RuntimeError, 'jie2.exe -> Qt6Core.dll'):
            deps.collect(self.package, [self.runtime], self.scan, True)
    def test_plugin_subdirectory_is_not_root_search_path(self):
        (self.package / 'platforms/Qt6Core.dll').touch()
        with self.assertRaisesRegex(RuntimeError, 'jie2.exe -> Qt6Core.dll'):
            deps.collect(self.package, [], self.scan, True)

if __name__ == '__main__': unittest.main()
