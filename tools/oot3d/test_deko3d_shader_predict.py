"""Regressão de deko3d_shader_predict.py com GLSL sintético (sem dados do jogo).

Bug original (23/11): a previsão só sabia varrer ~/oot3d-work/**/frag_*.glsl como fonte das partes de
cena. O pack previsto do MM3D mora em ~/oot3d-work/conv/mm3d-shaders, então a próxima rodada do OoT3D
leria os shaders do MM3D como "conhecidos" do OoT3D; e não havia como prever para outro título só com
os materiais dele.

    python3 -m unittest tools/oot3d/test_deko3d_shader_predict.py
"""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import deko3d_shader_pack as pack  # noqa: E402


def tev(name: str) -> str:
    return (f'\n    precise vec3 color_output_0 = {name}.rgb;\n'
            f'    precise float alpha_output_5 = {name}.a;\n'
            f'    combiner_output = vec4(color_output_0, alpha_output_5);\n')


def scene(head: str, material: str) -> str:
    return f'#version 430\n// {head}\nvoid main() {{{tev(material)}    color = combiner_output;\n}}\n'


def key(text: str) -> str:
    return '%016x' % pack.shader_text_key(pack.FRAGMENT, text.encode())


def write_frag(directory: Path, text: str) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    (directory / f'frag_{key(text)}.glsl').write_text(text)


def run(home: Path, work: Path, *args: str) -> set[str]:
    env = dict(os.environ, HOME=str(home))
    subprocess.run([sys.executable, '-I', str(HERE / 'deko3d_shader_predict.py'), str(work), *args],
                   env=env, check=True, capture_output=True, text=True)
    out = work / (args[args.index('--saida') + 1] if '--saida' in args else 'v8glsl')
    return {f.stem[5:] for f in out.glob('frag_*.glsl')}


class PredictTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.home = root / 'casa'
        self.work = root / 'titulo'
        (self.work / 'gen').mkdir(parents=True)
        (self.work / 'gen' / 'm0000.glsl').write_text(scene('gerado', 'tex_mm'))
        (self.work / 'regs.txt.luz').write_text('')
        (self.work / 'srcglsl').mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def test_other_title_uses_given_scenes_and_only_its_materials(self):
        known = Path(self.tmp.name) / 'oot3d'
        write_frag(known, scene('cena_oot', 'tex_oot'))
        got = run(self.home, self.work, '--conhecidos', str(known), '--saida', 'mm3dglsl')
        self.assertEqual(got, {key(scene('cena_oot', 'tex_mm'))})

    def test_default_ignores_other_titles_under_conv(self):
        write_frag(self.home / 'oot3d-work' / 'bench' / 'shaders', scene('cena_oot', 'tex_oot'))
        write_frag(self.home / 'oot3d-work' / 'conv' / 'mm3d-shaders' / 'mm3dglsl', scene('cena_mm', 'tex_x'))
        got = run(self.home, self.work)
        # Cena do OoT3D x (material conhecido do OoT3D + material do gen/), mais o conhecido copiado.
        self.assertEqual(got, {key(scene('cena_oot', 'tex_oot')), key(scene('cena_oot', 'tex_mm'))})


if __name__ == '__main__':
    unittest.main()
