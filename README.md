# ESP32-S3 Autoclicker (USB HID + Go Captor)

Projeto que transforma um **ESP32-S3** em um dispositivo USB HID real (mouse + teclado) com:
- **Interface Web** hospedada no próprio ESP para criar e rodar sequências de ações.
- **Captura de coordenadas** através de um serviço auxiliar em Go (usando `xdotool` no Linux).
- Possibilidade de rodar **uma vez, N vezes ou em loop infinito**.
- **Exportar/Importar JSON** de sequências para backup/edição.

💡 Ideal para automação de testes, repetição de rotinas em software, ou simplesmente diversão com HID.

---

## ✨ Recursos

- Simula **mouse (left/right/middle)**: click, drag, movimento relativo.
- Suporte a **teclas e atalhos**: `ctrl+c`, `alt+f4`, `return`, `tab`, `f1...f12`.
- Entrada de **texto** como se fosse teclado físico.
- **Delay pós-ação configurável** (default: 1500 ms).
- Loop configurável: rodar **uma vez**, **N vezes** ou **infinito**.
- LED RGB de status:
  - 🔵 Azul = standby
  - 🟢 Verde = rodando
  - 🔴 Vermelho = parado
- Interface Web responsiva com botões para:
  - Adicionar TAP/DRAG/TYPE/KEY
  - Exportar/Importar sequências
  - Salvar config (resolução, counts/px, delays)
  - Rodar/parar sequências
