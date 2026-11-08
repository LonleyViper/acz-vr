import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('datapack_model_refs', Path(__file__).with_name('inspect_datapack_model_refs.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def record(name):
    raw = name.encode('ascii')
    return raw + b'\0' * (32 - len(raw)) + module.SUFFIX


class ModelReferenceTests(unittest.TestCase):
    def test_contiguous_aircraft_run_and_false_match(self):
        items = ['mis/mpl/j35j.pmb', 'plmodel/itesu.mpl', 'mis/mpl/j35j.pvb'] + ['asset/foo.bin'] * 14
        data = b'prefix' + b''.join(record(item) for item in items) + b'mis/mpl/j35j.pmb'
        result = module.inspect(data, 'j35j')
        self.assertEqual(len(result['matches']), 1)
        self.assertEqual(result['matches'][0]['offset'], 6)
        self.assertEqual(result['matches'][0]['model_path'], 'plmodel/itesu.mpl')
        self.assertEqual(len(result['rejected_offsets']), 1)

    def test_bad_record_and_id_refused(self):
        self.assertIsNone(module.path_record(record('plmodel/itesu.mpl')[:-1], 0))
        with self.assertRaises(ValueError):
            module.inspect(b'', '../j35j')


if __name__ == '__main__':
    unittest.main()
