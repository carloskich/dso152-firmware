# Notas de versão

## v2.1 — 17/11/2026

**Aparelho**
- **Multímetro** (Menu → Multímetro): Vdc, Vrms, frequência, Vpp, máximo e mínimo ao mesmo tempo,
  um deles em destaque. Escala automática ou fixa (botão AUTO), HOLD (RUN), DC/AC (MODE) e ponta
  x1/x10 nas setas ◄ ►.
- **Aviso de tensão perigosa**: 60 V DC, 30 V RMS ou 42 V de pico em AC. Na ponta x1, aviso a partir
  de 35 V para usar x10. No multímetro o valor fica vermelho; no osciloscópio o aviso aparece no rodapé.
- **Média de capturas** (desligada, 4, 8 ou 16) para limpar o ruído de sinais repetitivos.
- **Posição do disparo** (pré-trigger): novo campo depois do trigger, marcador laranja no topo da
  grade. Parado (STOP), as setas percorrem a captura.
- Medidas novas: **tempo de subida** e **tempo de descida** (10 % a 90 %).
- **Gerador**: frequências fixas 10, 50, 60, 100 e 500 Hz, 1, 5, 10, 50 e 100 kHz; duty de 10 a 90 % no
  menu (submenu "Gerador de sinal"). Ajustar zero e restaurar zero passaram para "Sistema".

**PC**
- Gerador com frequência livre de 1 Hz a 200 kHz e duty de 1 a 99 %, com a frequência real gerada.
- Tela do multímetro e aviso de tensão perigosa também no PC.
- Espectro (FFT), decodificação UART (8N1), onda de referência e gravação de vídeo WebM.

## v2.0 — 16/11/2026

**Aparelho**
- Medidas à escolha nas cinco posições da barra de baixo (Menu → Medidas na tela):
  VPP, frequência, período, duty cycle, média, RMS verdadeiro, máximo e mínimo.
- Desligamento automático por inatividade: desligado, 5, 10 ou 30 min. Só na bateria;
  avisa 30 s antes e qualquer botão cancela.
- Brilho da tela: 100, 75, 50 ou 25 %.
- Menu reorganizado: submenus "Medidas na tela" e "Sistema" (brilho, desligamento
  automático, calibrar bateria, diagnóstico, Sobre).
- Tela **Sobre**: versão, autor, licença e novidades.
- As configurações da versão anterior (zero e calibração da bateria) são mantidas.

**PC**
- O menu do aparelho aparece na tela do PC e pode ser operado com cliques.
- Clique nos valores da tela para escolher em uma lista; clique em RUN para parar ou continuar.
- Histórico das últimas 3000 capturas, com congelar e voltar.
- Exportar a tela em PNG e a onda em CSV.
- Registrador: gráfico de uma medida ao longo do tempo, com estatísticas e exportação CSV.
- Tela cheia em alta resolução e formato wide; janela Sobre.

## v1.9 — 16/11/2026
- USB: o aparelho vira uma porta serial (CDC) e envia a tela para o PC. Ícone do PC com três
  estados: sem PC, PC com a porta fechada, transmitindo.
- Página do PC com a mesma tela e os mesmos botões do aparelho.
- Modo rolagem de 50 ms/div a 2 s/div.
- Cursores de tempo (ΔT, 1/ΔT) e de tensão (C1, C2, ΔV); o cursor selecionado fica em rosa.
- Calibração da bateria com tela própria (OK calibra, MODE volta ao padrão).
- v1.9.1: o resultado do ajuste de zero fica na tela até apertar um botão.
- v1.9.4: ícone do gerador cinza com X vermelho quando desligado.
- v1.9.5: AUTO mede a amplitude em uma janela curta e outra longa e acerta sinais lentos
  de primeira; sem sinal, vai para 100 mV/div e 1 ms/div.

## v1.8
- A linha guia do zero e do trigger é apagada corretamente, também com o aparelho parado.
- O ícone da bateria atualiza em STOP (ao ligar ou tirar o carregador).

## v1.7
- Marcadores de zero (esquerda) e de trigger (direita) ajustáveis e destacados quando selecionados.

## v1.6
- Menu mais seguro: as setas só navegam e OK altera o valor.
- Animação de carga da bateria; diagnóstico com versão, chip, tempo ligado, bateria e VDD.

## v1.5
- Textos em português com acentos; fonte 5×7 clássica em todas as telas.

## v1.4
- Gerador de sinal no menu (10 Hz a 100 kHz), leitura da bateria, aviso "FORA DA ESCALA".

## v1.0 – v1.3
- Primeira versão completa: captura, trigger, 11 faixas com ganho calibrado com multímetro,
  zero de fábrica e ajuste de zero, configurações salvas na flash, novo mapa de botões.
- A fonte que o bootloader da FNIRSI usa para a própria tela é preservada.

## v0.1 – v0.6
- Testes de hardware: relógio de 72 MHz, trava de alimentação, tela, botões e ADC.
