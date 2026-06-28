# ZMK Devtool Web Frontend

This React app connects to a ZMK keyboard over Studio serial transport and calls the `cormoran__devtool` custom Studio RPC subsystem.

## Commands

```bash
npm install
npm run generate
npm run dev
npm run build
npm test
```

## Project Structure

```text
src/
├── main.tsx
├── App.tsx
├── App.css
└── proto/
    └── cormoran/devtool/devtool.ts

test/
├── App.spec.tsx
└── RPCTestSection.spec.tsx
```

## Protocol

The protobuf schema is defined in `../proto/cormoran/devtool/devtool.proto`. Run `npm run generate` after editing the schema.

The UI exposes buttons for:

- Unlock Studio
- Lock Studio
- Get Lock State
- Enter Bootloader
- Reboot
