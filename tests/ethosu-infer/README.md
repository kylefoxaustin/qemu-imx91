# Ethos-U65 end-to-end inference (fork-only demo)

This runs a **real, correct neural-network inference** end to end on the QEMU
i.MX93 machine — from a Linux user-space app, through the Cortex-M33 ethos
firmware, to the modelled Ethos-U65 NPU and back — and prints the (correct)
classification.

```
guest ethosu_infer ──(ioctls)──► /dev/ethosu0
   │  open() boots the M33 on demand (i.MX SiP RPROC SMC the machine services)
   │  NETWORK_CREATE(Vela model) → INFERENCE_CREATE(IFM,OFM) → INVOKE  ──(MU/rpmsg)──►
   ▼                                                                    M33 firmware
   guest reads the OFM ◄── firmware copies output ◄── NPU completion IRQ (NVIC 178)
                                                          ▲
   QEMU NPU model (hw/misc/imx93_ethosu.c): on the firmware's command-stream
   kick it reads the guest IFM from the tensor arena, runs the *reference* int8
   TFLite model on the host, writes the exact OFM back, and raises the IRQ.
```

The demo model is a tiny 16x16 int8 CNN that classifies "top half brighter"
(class 0) vs "bottom half brighter" (class 1), trained + Vela-compiled on the
host. Two sample inputs are provided; each produces the correct, distinct
result (proving it is a genuine per-input inference, not a canned value).

## Why "host inference"

The real Ethos-U65 command-stream compute engine is not modelled (that is a
multi-month effort). Instead the NPU model runs the reference int8 TFLite model
on the host — the same numerics the silicon produces — and writes the result
into guest memory. This yields a correct end-to-end inference on the fork.
Because it shells out to a host Python helper, it is **not** an upstream QEMU
deliverable; it lives here purely as a showcase.

## Host prerequisites (one time)

The model build uses TensorFlow + Vela; the runtime helper uses tflite-runtime.
Note `numpy<2` is required (tflite-runtime's ABI):

```bash
pip install --user "tensorflow-cpu" "ethos-u-vela==4.3.0" tflite-runtime "numpy==1.26.4"
```

## Build the host assets

```bash
./host/build.sh      # -> host/model_int8{,_vela}.tflite, host/sample_{top,bottom}.bin
```

## Run

```bash
./run.sh                              # top sample  -> class 0
IFM=host/sample_bottom.bin ./run.sh   # bottom sample -> class 1
ETHOSU_TRACE=1 ./run.sh               # also dump NPU registers + command stream
```

PASS = `RESULT: INFERENCE OK` with the OFM matching the host golden and no
kernel oops. The QEMU process needs `ETHOSU_HOST_INFER` (the helper) and
`ETHOSU_HOST_MODEL` (the int8 reference) in its environment; `run.sh` sets them.

## Files

| file | role |
|---|---|
| `ethosu_infer.c`              | guest app: full BUFFER/NETWORK/INFERENCE/INVOKE ioctl flow |
| `host/make_model.py`          | trains + int8-quantizes the demo CNN, emits sample inputs |
| `host/ethosu_host_infer.py`   | host TFLite reference the QEMU NPU model execs on a kick |
| `host/build.sh`               | make_model.py + Vela → the canonical model assets |
| `run.sh`                      | stage assets, boot the on-demand path, run + check |
