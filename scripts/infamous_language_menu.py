"""Prelaunch language chooser using the existing Windows Python's Tkinter."""
import argparse
import json
from pathlib import Path
import sys
from game_profiles import console_locale,validate_console_locale

ROOT=Path(__file__).resolve().parent.parent
LANGUAGES=(('繁體中文（香港）',10,'chinese'),('English',1,'english'),('한국어（韩语）',9,'korean'))
BUTTONS=(('○ 确认 / Xbox B','circle'),('× 确认 / Xbox A','cross'))

def available_languages(game):
    cache=Path(game)/'art/cache'
    return [(name,code) for name,code,asset in LANGUAGES if (cache/f'lang_{asset}_text.xpps').is_file()]

def save_selection(data,language,button,show_fps=None):
    if language not in {code for _,code,_ in LANGUAGES} or button not in {code for _,code in BUTTONS}:
        raise ValueError('Unsupported language or confirm button')
    data=Path(data); locale=console_locale(data)
    locale.update(region='HK',system_language=language,timezone_minutes=480,confirm_button=button)
    if show_fps is not None: locale['show_fps']=show_fps
    validate_console_locale(locale)
    target=data/'infamous-locale.json'; pending=target.with_name(target.name+'.next')
    pending.write_text(json.dumps(locale,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    pending.replace(target)

class LanguageMenu:
    def __init__(self,data,game,root=None):
        import tkinter as tk
        from tkinter import ttk
        self.data=Path(data); self.game=Path(game)
        self.locale=console_locale(data); self.options=available_languages(game)
        if not self.options: raise ValueError('未找到游戏的语言资源，请先检查游戏目录。')
        self.root=root or tk.Tk(); self.result=1
        self.root.title('inFAMOUS Second Son · 启动设置')
        self.root.resizable(False,False)
        content=ttk.Frame(self.root,padding=24); content.grid(sticky='nsew')
        ttk.Label(content,text='inFAMOUS Second Son',font=('Segoe UI',16,'bold')).grid(row=0,column=0,columnspan=2,sticky='w',pady=(0,18))
        ttk.Label(content,text='游戏语言').grid(row=1,column=0,sticky='w',padx=(0,18))
        self.language=ttk.Combobox(content,state='readonly',width=26,values=[name for name,_ in self.options])
        self.language.grid(row=1,column=1,sticky='ew')
        current=next((i for i,(_,code) in enumerate(self.options) if code==self.locale['system_language']),0)
        self.language.current(current)
        ttk.Label(content,text='确认按键').grid(row=2,column=0,sticky='w',pady=(14,0))
        self.button=ttk.Combobox(content,state='readonly',width=26,values=[name for name,_ in BUTTONS])
        self.button.grid(row=2,column=1,sticky='ew',pady=(14,0))
        self.button.current(0 if self.locale['confirm_button']=='circle' else 1)
        self.show_fps=tk.BooleanVar(value=self.locale.get('show_fps',True))
        self.fps_checkbox=ttk.Checkbutton(content,text='在游戏左上角显示帧数（FPS）',variable=self.show_fps)
        self.fps_checkbox.grid(row=3,column=0,columnspan=2,sticky='w',pady=(16,0))
        self.motion_help=ttk.LabelFrame(content,text='鼠标代替手柄体感 / 触摸板',padding=12)
        self.motion_help.grid(row=4,column=0,columnspan=2,sticky='ew',pady=(16,0))
        ttk.Label(self.motion_help,text='F6  开关体感：移动控制方向，来回移动模拟摇晃。\n体感模式按住左键喷漆；F7 回正。\nF8  开关触摸板：左键点击或按住拖动模拟触摸/滑动，\n右键按下触摸板；F7 将触摸位置回到中央。\n两种模式互斥；Esc 或切换窗口释放鼠标。',justify='left',wraplength=380).pack(anchor='w')
        ttk.Label(content,text='地区：香港（HK）    时区：UTC+8\n语音：英语\n选择会自动保存，下次启动继续使用。',justify='left').grid(row=5,column=0,columnspan=2,sticky='w',pady=18)
        self.error=tk.StringVar()
        ttk.Label(content,textvariable=self.error,foreground='#b00020',wraplength=380).grid(row=6,column=0,columnspan=2,sticky='w')
        actions=ttk.Frame(content); actions.grid(row=7,column=0,columnspan=2,sticky='e',pady=(12,0))
        self.cancel_button=ttk.Button(actions,text='取消',command=self.cancel); self.cancel_button.pack(side='left',padx=(0,10))
        self.start_button=ttk.Button(actions,text='启动游戏',command=self.start); self.start_button.pack(side='left')
        self.root.protocol('WM_DELETE_WINDOW',self.cancel)
        self.root.bind('<Escape>',lambda _:self.cancel())
        self.root.bind('<Return>',lambda _:self.start())
        self.language.focus_set()
    def start(self):
        try:
            save_selection(self.data,self.options[self.language.current()][1],BUTTONS[self.button.current()][1],self.show_fps.get())
        except (OSError,ValueError) as error:
            self.error.set(f'无法保存设置：{error}'); return
        self.result=0; self.root.destroy()
    def cancel(self):
        self.result=1; self.root.destroy()
    def run(self): self.root.mainloop(); return self.result

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data',type=Path,default=ROOT)
    parser.add_argument('--game',type=Path,default=ROOT/'patches/CUSA00309')
    args=parser.parse_args()
    try:
        import tkinter as tk
        return LanguageMenu(args.data,args.game).run()
    except ImportError as error:
        print(f'现有 Python 的 Tkinter 不可用：{error}。未安装任何依赖；可直接编辑 infamous-locale.json。',file=sys.stderr); return 2
    except (OSError,ValueError) as error:
        print(error,file=sys.stderr); return 2
    except tk.TclError as error:
        print(f'无法创建语言设置窗口：{error}',file=sys.stderr); return 2

if __name__=='__main__': sys.exit(main())
