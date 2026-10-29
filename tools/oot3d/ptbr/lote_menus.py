"""Lista de rótulos PT-BR por textura (índices das regiões de regioes.py)."""
EQ = 'EQUIP.'
SIM_NAO = {}
LOTE = {
    'menu_file_select_parts00': ({0: 'Apagar', 1: 'Apagar', 4: 'Sim', 5: 'Sobrescrever', 6: 'Não', 7: 'Voltar',
                                  8: 'Começar', 9: 'Opções'}, {}),
    'menu_file_select_parts01': ({0: 'Apagar', 1: 'Apagar', 4: 'Sim', 5: 'Sobrescrever', 6: 'Não', 7: 'Voltar',
                                  8: 'Começar', 9: 'Opções'}, {}),
    'menu_top_parts00': ({2: 'Sair', 3: EQ}, {}),
    'hud_menu_title00': ({1: 'Salvar', 2: EQ, 4: 'Digite o nome', 5: 'Apagar dados', 6: 'Copiar dados',
                          7: 'Opções', 8: 'Escolha um arquivo', 9: 'LOCAL ATUAL'}, {'peso': 'serifa', 'fator': 0.62, 'placa': True}),
    'menu_option_parts00': ({0: 'Segurar', 1: 'FIXAR ALVO', 2: 'Alternar', 4: 'Sim', 6: 'Inverter eixo Y',
                             ('caixa', 118, 48, 256, 71): 'CÂMERA', ('caixa', 118, 72, 256, 95): 'MOVIMENTO',
                             7: 'Não', 8: 'Sim', 9: 'Não', 10: 'Opções', 11: 'Aceitar', 13: 'Voltar'}, {}),
    'menu_hint_movie_parts00': ({2: 'Voltar', 3: 'Visões'}, {}),
    'menu_equip_parts00': ({1: 'Sair', 2: 'Voltar', 3: EQ}, {}),
    'menu_item_parts00': ({0: EQ, 3: 'Voltar'}, {}),
    'menu_okarina_parts00': ({0: 'Sair'}, {}),
    'name_entry00_parts01': ({0: 'Voltar', 1: 'Sim', 2: 'Aceitar', ('caixa', 401, 239, 440, 258): 'Não'}, {}),
}

C = lambda *c: ('caixa',) + c
MAPAS = {
    'menu_map_parts00': ({C(97, 90, 248, 108): 'Grande Árvore Deku', C(97, 115, 248, 134): 'Caverna dos Dodongos',
                          C(97, 139, 248, 158): 'Barriga do Jabu-Jabu', C(97, 163, 248, 182): 'Templo da Floresta',
                          C(186, 187, 336, 207): 'Templo do Fogo', C(186, 211, 336, 233): 'Templo da Água',
                          C(186, 235, 336, 256): 'Templo do Espírito', C(344, 187, 500, 207): 'Templo das Sombras',
                          C(344, 211, 500, 231): 'Fundo do Poço', C(344, 235, 500, 252): 'Caverna de Gelo'},
                         {'peso': 'serifa', 'altura': 11, 'alinhar': 'esquerda', 'transparente': True,
                          'cores': ((255, 255, 255, 255), (0, 0, 0, 255))}),
    'menu_map_parts01': ({C(329, 64, 401, 80): 'Floresta', C(329, 80, 401, 95): 'Kokiri',
                          C(329, 97, 401, 111): 'Domínio', C(329, 111, 401, 126): 'Zora',
                          C(424, 1, 512, 16): 'Deserto', C(424, 16, 512, 30): 'Assombrado',
                          C(424, 33, 512, 48): 'Fortaleza', C(424, 48, 512, 64): 'Gerudo',
                          C(416, 73, 512, 88): 'Vale Gerudo', C(416, 105, 512, 122): 'Lago Hylia',
                          C(424, 161, 512, 176): 'Praça do', C(424, 176, 512, 190): 'Mercado',
                          C(424, 193, 512, 208): 'Campo', C(424, 207, 512, 223): 'de Hyrule',
                          C(424, 224, 512, 240): 'Montanha', C(424, 239, 512, 254): 'da Morte'},
                         {'altura': 10, 'alinhar': 'esquerda', 'transparente': True,
                          'cores': ((255, 255, 255, 255), (0, 0, 0, 255))}),
}

BOTOES_MAPAS = {'menu_map_parts00': {1: 'Voltar', 14: EQ}, 'menu_map_parts01': {6: EQ, 10: 'Voltar'}}
