import { IFamCanvas, KeyboardPad, Mapper, NesFile, PPUCanvas, PPUCanvasWebGL, WorkerSound } from "nes-emu";
import { useEffect, useRef, useState } from "react";

import "./FamMain.css";

export type DisplayMode = "original" | "scale" | "ntsc";

function FamMain({ mode = "original", clip = true }: { mode: DisplayMode; clip?: boolean; }) {
    const canvasRef = useRef<HTMLCanvasElement>(null);
    const [rom, setRom] = useState<Mapper | null>(null);
    const [crtWidth, setCrtWidth] = useState(256);
    const [crtHeight, setCrtHeight] = useState(224);
    useEffect(() => {
        if (mode === "original") {
            setCrtWidth(256);
            setCrtHeight(clip ? 224 : 240);
        } else {
            setCrtWidth(512);
            setCrtHeight(clip ? 448 : 480);
        }
    }, [mode, clip]);
    const requestBodyFocus = () => {
        setTimeout(() => { document.body.focus(); });
    }
    const powerOff = () => {
        if (rom) {
            rom.powerOff();
            setRom(null);
        }
        requestBodyFocus();
    };
    const reset = () => {
        if (rom) {
            rom.reset();
        }
        requestBodyFocus();
    };
    const handleFileClick = () => {
        if (rom) {
            rom.stopPlay();
        }
    };
    const handleFileChange = async (event: any) => {
        const file = event.target.files[0]; // ファイルを取得
        requestBodyFocus();
        if (file) {
            if (rom) {
                rom.powerOff();
            }
            try {
                // ファイルをバイナリとして読み込む
                const arrayBuffer = await file.arrayBuffer();
                const uint8Array = new Uint8Array(arrayBuffer);

                // 結果を state に保存
                const nes = new NesFile(uint8Array);
                console.log(nes);
                const mapper = Mapper.getMapper(nes);
                let canvas: IFamCanvas;
                if (mode === "ntsc") {
                    canvas = new PPUCanvasWebGL(canvasRef.current!);
                } else {
                    canvas = new PPUCanvas(canvasRef.current!);
                }
                await mapper.init(canvas, await WorkerSound.getSound());
                mapper.setPad(0, new KeyboardPad());
                setRom(mapper)
                /*
                let counter = 0;
                let debugFlag = false;
                mapper.setDebugCallback(obj => {
                  if (debugFlag) {
                    if (counter > -2000) {
                      console.log(obj.toString());
                    }
                    counter++;
                    if (counter > 10000) {
                      mapper.stopPlay();
                    }  
                  } else if (obj.pc == 0xc4491) {
                    console.log(obj.toString());
                    debugFlag = true;
                  }
                });
                */
                mapper.startPlay();
            } catch (error) {
                console.error("ファイル読み込みエラー:", error);
            }
        } else if (rom) {
            rom.startPlay();
        }
    };

    return (
        <>
            <div className='crt-area'>
                <div className="crt-container">
                    <div className={"crt-screen" + (mode === 'ntsc' ? ' crt-ntsc': '')}>
                        <canvas ref={canvasRef} id="gameCanvas" width={crtWidth} height={crtHeight}></canvas>
                        <div className="crt-overlay"></div>
                    </div>
                </div>
            </div>

            <div className='fam-container'>
                <div className="controller">
                    <div className="dpad">
                        <div></div><div className="up"></div><div></div>
                        <div className="left"></div><div className="center"></div><div className="right"></div>
                        <div></div><div className="down"></div><div></div>
                    </div>
                    <div className="start-select">
                        <div className="start"></div>
                        <div className="select"></div>
                    </div>
                    <div className="ct-buttons">
                        <div className="b-button"></div>
                        <div className="a-button"></div>
                    </div>
                </div>

                <div className="famicom">
                    <div className="cartridge-slot">Insert Cartridge<input type="file" onClick={handleFileClick} onChange={handleFileChange} /></div>

                    <div className="buttons">
                        <button onClick={powerOff} className="power-button">Power</button>
                        <button onClick={reset} className="reset-button">Reset</button>
                    </div>
                </div>

                <div className="controller">
                    <div className="dpad">
                        <div></div><div className="up"></div><div></div>
                        <div className="left"></div><div className="center"></div><div className="right"></div>
                        <div></div><div className="down"></div><div></div>
                    </div>
                    <div className="ct-buttons">
                        <div className="b-button"></div>
                        <div className="a-button"></div>
                    </div>
                </div>
            </div>
        </>
    )
}

export default FamMain;
