# Qualified entry memory serial fixtures

Archived from the isolated native runner on 2026-09-21. These fixtures exercise
its acceptance parser only; replaying text does not prove a fresh native run.
Four profiles explicitly have no qualified map and perform no page writes.
Complete per-run receipts, ELF images and sources are retained separately.

```json
[
  {
    "architecture": "i386",
    "sha256": "6691a485aa07f15b3e73245a46ea8628cdb6f8b49e8b172d71a6f6112c056337",
    "result_sha256": "bfde325b761eca7111a2de65151c6175d097ffa20808c8d21b736767f2bfea7c"
  },
  {
    "architecture": "x86_64",
    "sha256": "d490f1b8fe14f425329464f60fbf59106fecd4ec0b190e2ccd0501459844d165",
    "result_sha256": "acf41a13c98cdbef035d76e18abc7fe35cc83ea24a50b93ae543d099b6a26012"
  },
  {
    "architecture": "aarch64",
    "sha256": "304140f36e2711cfcbcd8910d819595b64c332bd8e3be96516205e98350c4d46",
    "result_sha256": "04c0394fc566474669ee5aeed3b7755c164aad376a88fb171fa90e07a9986854"
  },
  {
    "architecture": "arm32",
    "sha256": "ef8abe31291395d84bc50100a95d38567cbfe38e12cef116f349d77aa41d999a",
    "result_sha256": "4acba5f908f0072c9249ac3019f2e1f82a96dc60d912494a14dcb2adc6421e26"
  },
  {
    "architecture": "rv64",
    "sha256": "faaf9f94686fb20c4c83b63718621a75108d17e9d6ec8dbc3964dddc6faa9b1e",
    "result_sha256": "33660fad21cfc3b48fd558d03e7d55d286bab43920e1db0612a78daa392a1c45"
  },
  {
    "architecture": "rv32",
    "sha256": "a15583b789faf62a35680659dab9ddfb1d93f6dc2ce8592b0d8f04bd5e503fac",
    "result_sha256": "3f1646514e8379576f029c95140bf0d4dcbdb2f298acc14c65432d371f6b20ff"
  },
  {
    "architecture": "loongarch64",
    "sha256": "4e36c37d76296496526749a337aa5dbb4c2eca0ba568bb9d506a1c2dd40db812",
    "result_sha256": "ef416143220872d54dabe8e3c0d783c76610fae687da16746858d187ce37acca"
  },
  {
    "architecture": "mips",
    "sha256": "927e154c0f6ca80a9ac445181f02d7943baf8730a9ee2333942b4416ab4abe94",
    "result_sha256": "e41ef3bcc2c59c57b4bbbe9b225039c5e31ab3f94c5d78547b5fdee903d4219d"
  },
  {
    "architecture": "mips64",
    "sha256": "2bbee52272964e1b2e91f69cf8c963309f0570b63e7f07830dca9aa0a2ef5684",
    "result_sha256": "624a1ef1120a76161e7e6ba2a2438743a59194d9cce069dab953f1359c9e0c76"
  },
  {
    "architecture": "ppc",
    "sha256": "c248b1dbef5d9ba60f01173e5530d7cb0ad50ef55cebd9a84f590f72d87a972a",
    "result_sha256": "1c777c51d4dc820d52a55bb934da2faf4cd58e2075077301026c57cb8181355a"
  }
]
```
