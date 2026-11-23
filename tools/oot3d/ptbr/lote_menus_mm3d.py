"""Rótulos PT-BR das texturas de menu do MM3D (layout/EU_Spanish/*.gar), por membro do GAR.

Cada rótulo: (caixa do texto espanhol x0, y0, x1, y1, 'PT', opções). Opções: 'livre' = (x esquerdo, x direito) até
onde o PT pode ir (padrão: a caixa com 4 px de folga), 'maiusc' = texto só de maiúsculas (mede pela altura do H),
'grade' = grupo de células iguais (altura-x e base: mediana do grupo), 'cores' = (letra, contorno) quando a detecção erra,
'xh'/'base' = altura-x e base explícitas (px), 'camadas' = contornos [(cor, espessura)] de fora para dentro,
'folga' = px apagados além da caixa (1 perto de ícones), 'fundo' = cor lisa no lugar do texto,
'peso' = peso fixo da Noto Sans CJK (senão o de traço mais parecido com o espanhol).
Caixas medidas no espanhol decodificado (mm3d_texturas.py extrair + rotulos_menu, conferidas em folhas com grade).
PT: falas do jogo (eus-ptbr.gmsg) e glossário/manual PT; nomes de lugar vêm da tabela dos mapas (tabela-es-pt.json).
Ficam como estão: o que é igual em PT (Máscaras, Mapa, Final, Normal, Alto, Pág. 1, Eventos, Copiar, Página, Ver),
os ordinais (1.°, 2.°: decisão na pesquisa) e o aviso do botão HOME da tela de início.
"""
# HUD do botão de ação: grade de 64 x 18 px a partir de y=93 (como o hud_all00 do OoT3D); colunas na ordem das
# células do inglês (Attack, Check, Barrier, ...), que é a mesma do espanhol
HUD_ACAO = [
    ['Atacar', 'Olhar', 'Barreira', 'Voltar', 'Abrir', 'Pular', 'Escolher', 'Mergulhar', 'Galopar'],
    ['Lançar', 'Rolar', 'Subir', 'Soltar', 'Descer', 'Salvar', 'Falar', 'Próximo', 'Agarrar'],
    ['Cancelar', 'Guardar', 'Recolher', 'Info', 'Voar', 'Foto', 'Explodir', 'Dançar', 'Marchar'],
    ['Enrolar', 'Emergir', 'Nadar', 'Socar', 'Esmagar', None, 'Atirar', None, None],
    ['Mergulhar', 'Parar'],
]


def _hud():
    out = []
    for c, col in enumerate(HUD_ACAO):
        for r, pt in enumerate(col):
            if pt:
                y = 93 + 18 * r
                out.append((64 * c + 2, y + 1, 64 * c + 62, y + 18, pt, {'livre': (64 * c, 64 * c + 64), 'grade': True}))
    return out


LOTE = {
    'the_end_text.ctxb': [(40, 3, 90, 30, 'Fim', {'livre': (28, 102), 'fundo': (0, 0, 0, 255), 'maiusc': True, 'xh': 21, 'base': 27,
                                    'cores': ((240, 240, 240, 255), None), 'camadas': [((110, 110, 110, 255), 1.0)]})],
    'menu_file_select_parts00.ctxb': [
        (349, 13, 452, 31, 'Começar', {'livre': (300, 500)}), (414, 50, 483, 68, 'Opções', {'livre': (330, 505)}),
        (443, 123, 501, 138, 'Apagar', {'livre': (432, 510)})],
    'btn_return.ctxb': [(10, 49, 61, 63, 'Voltar', {'livre': (6, 62), 'grade': 'b'}), (66, 49, 118, 63, 'Sair', {'livre': (66, 124), 'grade': 'b'})],
    'menu_common_parts00.ctxb': [
        (0, 35, 143, 55, 'Canção do Tempo Duplo', {'livre': (0, 144), 'alinhar': 'esq'}), (145, 35, 220, 55, 'Visões', {'livre': (144, 280), 'grade': 'menu', 'alinhar': 'esq'}),
        (28, 58, 96, 79, 'Horário', {'livre': (28, 143), 'grade': 'menu', 'alinhar': 'esq'}), (144, 58, 210, 79, 'Inventário', {'livre': (144, 286), 'grade': 'menu', 'alinhar': 'esq'}),
        (28, 82, 144, 103, 'Lista de Eventos', {'livre': (28, 144), 'grade': 'menu', 'alinhar': 'esq'}),
        (146, 107, 235, 125, 'Itens', {'livre': (144, 286), 'grade': 'menu', 'alinhar': 'esq'}), (289, 59, 371, 80, 'Opções', {'livre': (287, 432), 'grade': 'menu', 'alinhar': 'esq'}),
        (287, 84, 431, 103, 'Para onde voar?', {'livre': (287, 432), 'grade': 'menu', 'alinhar': 'esq'}), (289, 107, 352, 125, 'Alarme', {'livre': (287, 432), 'grade': 'menu', 'alinhar': 'esq'})],
    'menu_popup_parts00.ctxb': [
        (68, 10, 85, 24, 'Sim', {'livre': (22, 130), 'grade': 'botao'}), (173, 10, 220, 24, 'Apagar', {'livre': (142, 248), 'grade': 'botao'}),
        (67, 46, 89, 60, 'Não', {'livre': (60, 128), 'grade': 'botao'}), (166, 45, 228, 62, 'Não usar', {'livre': (142, 248), 'grade': 'botao'}),
        (179, 81, 228, 96, 'Sair', {'livre': (179, 246), 'grade': 'botao'}),
        (3, 115, 143, 133, 'Digite seu nome', {'livre': (0, 150), 'grade': 'lista', 'alinhar': 'esq'}), (3, 139, 127, 157, 'Apagar arquivo', {'livre': (0, 150), 'grade': 'lista', 'alinhar': 'esq'}),
        (2, 163, 128, 184, 'Copiar arquivo', {'livre': (0, 150), 'grade': 'lista', 'alinhar': 'esq'}), (3, 187, 122, 208, 'Escolher arquivo', {'livre': (0, 150), 'grade': 'lista', 'alinhar': 'esq'}),
        (27, 210, 125, 232, 'Dica em vídeo', {'livre': (27, 150), 'grade': 'lista', 'alinhar': 'esq'}), (27, 235, 115, 256, 'Dica em foto', {'livre': (27, 150), 'grade': 'lista', 'alinhar': 'esq'}),
        (154, 154, 236, 168, 'Sobrescrever', {'livre': (152, 250), 'grade': 'botao'})],
    'parts.ctxb': [(9, 81, 89, 93, 'Dia todo', {'livre': (0, 95), 'grade': 'p'}),
                   (14, 98, 179, 112, 'Amanhecer do Segundo Dia', {'livre': (4, 190), 'grade': 'p'}),
                   (25, 114, 167, 125, 'Amanhecer do Último Dia', {'livre': (4, 190), 'grade': 'p'})],
    'logo_countdown.ctxb': [(26, 55, 102, 96, 'Já!', {'livre': (0, 127), 'maiusc': True, 'xh': 30, 'base': 93,
                                    'cores': ((20, 165, 230, 255), None), 'camadas': [((0, 0, 0, 255), 3.0)]})],
    'hud_patrs00_action_text.ctxb': _hud() + [
        (257, 0, 306, 15, 'Tatl', {'livre': (256, 330), 'grade': 'tatl'}), (257, 16, 306, 31, 'Alarme', {'livre': (256, 330), 'grade': 'tatl'}),
        (257, 32, 292, 47, 'Dica', {'livre': (256, 330), 'grade': 'tatl'}),
        (353, 35, 398, 50, 'Inventário', {'livre': (352, 402)}), (369, 164, 430, 180, 'Itens', {'livre': (366, 432), 'xh': 6.5, 'cores': ((240, 240, 240, 255), (20, 30, 60, 255))}),
        (350, 193, 435, 215, 'Sem mapa', {'livre': (346, 436), 'cores': ((205, 205, 205, 255), (0, 0, 0, 255))}), (443, 198, 490, 213, 'Voltar', {'livre': (438, 492)})],
    'menu_hint_movie_parts00.ctxb': [(460, 21, 512, 34, 'Próximo', {'livre': (456, 512), 'fundo': (255, 100, 24, 255), 'xh': 5.5, 'cores': ((250, 250, 250, 255), (60, 10, 0, 255))}),
                                     (454, 68, 505, 87, 'Feito!', {'livre': (448, 512), 'xh': 7, 'cores': ((20, 150, 30, 255), None), 'camadas': [((0, 0, 0, 255), 2.2), ((255, 255, 255, 255), 1.2)]})],
    'menu_hint_movie_parts01.ctxb': [(6, 0, 57, 19, 'Feito!', {'livre': (1, 63), 'xh': 7, 'cores': ((20, 150, 30, 255), None), 'camadas': [((0, 0, 0, 255), 2.2), ((255, 255, 255, 255), 1.2)]})],
    'menu_option_parts01.ctxb': [(1, 0, 66, 13, 'Volume', {'livre': (1, 109), 'alinhar': 'esq', 'peso': 'Regular'}), (29, 18, 56, 32, 'Baixo', {'livre': (8, 77), 'peso': 'Regular'})],
    'menu_option_parts00.ctxb': [
        (0, 0, 139, 14, 'Mira com L', {'livre': (0, 144), 'grade': 'opcao', 'peso': 'Regular', 'alinhar': 'esq'}), (0, 16, 139, 30, 'Visão Subjetiva', {'livre': (0, 144), 'grade': 'opcao', 'peso': 'Regular', 'alinhar': 'esq'}),
        (0, 32, 139, 46, 'Controle de Movimento', {'livre': (0, 144), 'grade': 'opcao', 'peso': 'Regular', 'alinhar': 'esq'}),
        (0, 48, 139, 62, 'Controles de Nado', {'livre': (0, 144), 'grade': 'opcao', 'peso': 'Regular', 'alinhar': 'esq'}),
        (1, 64, 139, 77, 'Botão Deslizante Pro', {'livre': (0, 144), 'grade': 'opcao', 'peso': 'Regular', 'alinhar': 'esq'}), (0, 80, 88, 93, 'Câmera Livre', {'livre': (0, 144), 'grade': 'opcao', 'peso': 'Regular', 'alinhar': 'esq'}),
        (181, 2, 240, 13, 'Segurar', {'livre': (167, 253), 'grade': 'valor', 'peso': 'Regular'}), (185, 18, 237, 29, 'Alternar', {'livre': (167, 253), 'grade': 'valor', 'peso': 'Regular'}),
        (173, 51, 247, 63, 'Inverter eixo Y', {'livre': (165, 256), 'grade': 'valor', 'peso': 'Regular'}),
        (173, 67, 247, 79, 'Inverter eixo X', {'livre': (165, 256), 'grade': 'valor', 'peso': 'Regular'}),
        (173, 83, 240, 95, 'Inverter eixos', {'livre': (165, 256), 'grade': 'valor', 'peso': 'Regular'}),
        (186, 114, 237, 125, 'Não usar', {'livre': (172, 246), 'grade': 'valor', 'peso': 'Regular'}), (201, 130, 218, 141, 'Não', {'livre': (190, 230), 'grade': 'valor', 'peso': 'Regular'}),
        (204, 146, 215, 157, 'Sim', {'livre': (190, 230), 'grade': 'valor', 'peso': 'Regular'}),
        (0, 228, 223, 251, 'Calibrar o Botão Deslizante Pro', {'livre': (0, 226)})],
    'logo_perfect_parts00.ctxb': [(42, 0, 213, 32, 'PERFEITO!', {'livre': (31, 224), 'maiusc': True, 'xh': 22, 'base': 27,
                                   'cores': ((235, 180, 90, 255), None), 'camadas': [((40, 30, 20, 255), 2.5)]}),
                                  (42, 32, 213, 64, 'PERFEITO!', {'livre': (31, 224), 'maiusc': True, 'xh': 22, 'base': 60,
                                   'cores': ((250, 250, 170, 255), None), 'camadas': [((150, 225, 230, 255), 3.0), ((255, 255, 255, 255), 1.5)]})],
    'menu_collect_parts00.ctxb': [(413, 38, 482, 56, 'Opções', {'livre': (410, 486)}),
                                  (370, 233, 511, 256, 'Sem fotos', {'livre': (368, 512)})],
    'menu_item_parts00.ctxb': [(133, 7, 200, 22, 'Itens', {'livre': (128, 204), 'cores': ((240, 240, 240, 255), (20, 30, 60, 255))}),
                               (145, 70, 213, 85, 'Itens', {'livre': (140, 218), 'cores': ((240, 240, 240, 255), (20, 30, 60, 255))})],
    'menu_map_parts00.ctxb': [(144, 1, 168, 16, 'T', {'livre': (138, 174), 'maiusc': True}),
                              (180, 1, 199, 15, 'T', {'livre': (174, 209), 'maiusc': True}),
                              (88, 22, 125, 34, 'Escolher', {'livre': (80, 128), 'folga': 1, 'xh': 6, 'cores': ((255, 255, 255, 255), (90, 20, 10, 255))})],
    'menu_quest_list_parts00.ctxb': [(248, 0, 288, 12, 'Novo!', {'livre': (246, 300), 'xh': 6, 'base': 10, 'cores': ((208, 21, 4, 255), None), 'camadas': [((0, 0, 0, 255), 2.2), ((255, 255, 255, 255), 1.2)]}),
                                     (248, 24, 288, 36, 'Novo!', {'livre': (246, 300), 'xh': 6, 'base': 34, 'cores': ((240, 236, 90, 255), None), 'camadas': [((140, 240, 240, 255), 2.2), ((255, 255, 255, 255), 1.2)]}),
                                     (464, 114, 509, 126, 'Horário', {'livre': (460, 511), 'folga': 1, 'xh': 6, 'cores': ((240, 240, 240, 255), (20, 30, 60, 255))})],
    'menu_schedule_parts00.ctxb': [
        (289, 3, 322, 11, 'Novo', {'livre': (289, 322), 'fundo': (255, 255, 255, 255), 'xh': 5, 'base': 11, 'cores': ((208, 30, 20, 255), None), 'camadas': []}), (25, 24, 90, 42, '1º dia', {'livre': (4, 108), 'folga': 0, 'xh': 9, 'base': 40, 'cores': ((240, 240, 240, 255), (10, 20, 30, 255)), 'camadas': [((10, 20, 30, 255), 1.8)]}),
        (143, 24, 199, 42, '2º dia', {'livre': (120, 228), 'folga': 0, 'xh': 9, 'base': 40, 'cores': ((240, 240, 240, 255), (30, 10, 10, 255)), 'camadas': [((30, 10, 10, 255), 1.8)]}), (419, 43, 457, 54, 'Ativar', {'livre': (410, 468), 'folga': 1, 'cores': ((255, 255, 255, 255), (30, 24, 10, 255)), 'xh': 5.5}),
        (265, 92, 308, 104, 'Novo!', {'livre': (262, 312), 'xh': 6, 'base': 102, 'cores': ((240, 236, 90, 255), None), 'camadas': [((140, 240, 240, 255), 2.2), ((255, 255, 255, 255), 1.2)]}), (34, 115, 88, 126, 'Desativar', {'livre': (34, 90), 'folga': 1, 'cores': ((255, 255, 255, 255), (30, 24, 10, 255)), 'xh': 5.5}),
        (380, 117, 432, 130, 'Avançar', {'livre': (378, 434), 'fundo': (245, 102, 24, 255), 'cores': ((255, 255, 255, 255), (30, 24, 10, 255)), 'xh': 5.5}), (10, 161, 51, 172, 'Alarme', {'livre': (4, 58), 'folga': 1, 'cores': ((255, 255, 255, 255), (30, 24, 10, 255)), 'xh': 5.5})],
    'menu_startup_parts00.ctxb': [(29, 6, 178, 27, 'APERTE START', {'livre': (2, 231), 'maiusc': True, 'xh': 14.5, 'base': 24, 'peso': 'Regular'}),
                                  (28, 36, 179, 61, 'APERTE START', {'livre': (2, 231), 'maiusc': True, 'xh': 14.5, 'base': 55, 'peso': 'Regular'})],
}

# Mapa-múndi (world_map_base00): caixa e livre; o PT sai do nome espanhol pela tabela dos mapas e pelas falas
# (nomes de área), feito em mm3d_texturas.lote_mapa_mundi. Os nomes de região soltos vão direto.
MUNDI_SOLTOS = {'Océano': 'Oceano', 'Cañón': 'Cânion', 'Montaña': 'Montanha', 'Pantano': 'Pântano',
                'Rancho': 'Rancho', 'Ciudad': 'Cidade', 'Mapa de Términa': 'Mapa de Termina',
                'Alejar': 'Afastar', 'Acercar': 'Aproximar', 'Seleccionar': 'Escolher'}
MUNDI = [  # (caixa, livre) na ordem da transcrição mundi-es.json; grupos de tamanho em MUNDI_GRUPO
    ((12, 0, 131, 18), (0, 147)), ((164, 0, 252, 18), (147, 263)), ((274, 0, 402, 18), (263, 402)),
    ((0, 19, 143, 34), (0, 146)), ((150, 19, 267, 35), (146, 271)), ((275, 19, 402, 36), (271, 402)),
    ((45, 38, 99, 54), (0, 125)), ((152, 39, 266, 56), (125, 283)), ((300, 38, 388, 54), (283, 402)),
    ((1, 57, 115, 73), (0, 130)), ((145, 57, 273, 72), (130, 282)), ((291, 57, 384, 75), (282, 402)),
    ((20, 76, 125, 92), (0, 140)), ((155, 76, 263, 92), (140, 283)), ((303, 76, 376, 92), (283, 402)),
    ((1, 95, 143, 110), (0, 145)), ((146, 95, 273, 110), (145, 278)), ((283, 95, 393, 111), (278, 402)),
    ((17, 114, 127, 130), (0, 146)), ((165, 114, 254, 130), (146, 402)), ((47, 133, 98, 149), (0, 122)),
    ((146, 133, 251, 149), (122, 402)), ((0, 152, 144, 170), (0, 149)), ((155, 152, 272, 168), (149, 402)),
    ((421, 0, 484, 17), (412, 495)), ((425, 21, 480, 38), (412, 495)), ((417, 43, 487, 59), (412, 495)),
    ((418, 64, 486, 80), (412, 495)),
    ((0, 181, 192, 199), (0, 194)), ((14, 200, 178, 218), (0, 194)), ((11, 219, 180, 237), (0, 194)),
    ((0, 238, 191, 256), (0, 194)),
    ((214, 209, 267, 223), (213, 300)), ((214, 225, 272, 239), (213, 300)), ((214, 241, 290, 256), (213, 300)),
]

# grupo de tamanho de cada rótulo do mapa-múndi (mesma ordem): nomes, regiões grandes, templos, botões
MUNDI_GRUPO = ['nome'] * 24 + ['regiao'] * 4 + ['templo'] * 4 + ['botao'] * 3
MUNDI_AJUSTE = {1: {'descer': 1, 'crescer': -1}}   # "Cidade do Relógio" encosta no topo: o acento sumia
