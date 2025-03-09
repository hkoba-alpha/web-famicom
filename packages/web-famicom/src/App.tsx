import { useState } from 'react';
import './App.css'
import FamMain, { DisplayMode } from './FamMain';

function App() {
  const [mode, setMode] = useState<DisplayMode | "">("");
  const [clip, setClip] = useState(true); // 画面上下をクリップするかどうか

  return (
    <>
      {!mode && (
        <>
          <div className="controls">
            <h3>テレビの種類を選んでください</h3>
            <button onClick={() => setMode('original')}>小型液晶テレビ</button>
            <button onClick={() => setMode('scale')}>大型液晶テレビ</button>
            <button onClick={() => setMode('ntsc')}>大型ブラウン管テレビ</button>

            <div className="checkbox-container">
              <label>
                <input
                  type="checkbox"
                  checked={clip}
                  onChange={(e) => setClip(e.target.checked)}
                />
                画面上下をクリップ
              </label>
            </div>
          </div>


          <div className="explanation">
            <div className="explanation-item">
              <h4>小型液晶テレビ</h4>
              <p>ファミコンと同じ256x240あるいは256x224のピクセルで表示します。</p>
            </div>
            <div className="explanation-item">
              <h4>大型液晶テレビ</h4>
              <p>縦横それぞれ2倍の大きさで表示します。</p>
            </div>
            <div className="explanation-item">
              <h4>大型ブラウン管テレビ</h4>
              <p>昔のブラウン管テレビのようなエフェクトで表示します。</p>
            </div>
            <div className="explanation-item">
              <h4>画面上下をクリップ</h4>
              <p>ファミコンの仕様上は縦240ピクセルですが、画面に見えない上下8ピクセルずつをクリップして非表示します。</p>
            </div>
          </div>
        </>
      )}

      {mode && <FamMain mode={mode} clip={clip} />}
    </>
  );
}

export default App
