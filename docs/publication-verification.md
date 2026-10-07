# 公開ソースの確認（2026-10-07）

ESP32-SOLO-1向けBLE Central / DIN MIDI OUT、0.5.0シリアルコンソール版です。

- ESP-IDF v6.1、ターゲットesp32、UNICORE、4MB/DIO/40MHz。
- アプリケーションの機能コードは公開準備で変更していません。
- Windows / GCCによる新しいホストビルドでCTest **8/8 PASS**：parser、TX queue、OLED frame、peer policy、OLED transfer、device controls、MIDI merge、serial command。
- SSD1306表示例2点は実際の描画コードから生成した画像です。
- ソース、設定初期値、ホストテストを公開。ESP-IDF本体、ビルド生成物、実機のFlash/NVSバックアップ、認証情報、RP2040ファームウェアは含みません。

ホストテスト：

```sh
cmake -S test/host -B build-host -DCMAKE_C_COMPILER=gcc
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

実機の接続・UI・互換性の確認範囲や既知の制限はREADMEの各節を参照してください。公開準備時に機器を書き換えたり、登録機器を消去したりしていません。
