import os
import tkinter as tk
from tkinter import filedialog, messagebox, ttk
import re
from pathlib import Path
import subprocess
import sys
from collections import defaultdict
import datetime

class MapFileAnalyzer:
    def __init__(self, root):
        self.root = root
        self.root.title("Map文件中断向量表分析器 - 支持对比")
        self.root.geometry("1000x700")
        
        # 存储已加载的map文件信息
        self.loaded_maps = {}  # {文件路径: {内容, 向量信息, 显示名称}}
        self.current_selection = None
        
        # 搜索相关
        self.search_windows = {}  # 为每个文本控件保存搜索窗口
        
        # 设置拖放功能
        self.setup_drag_drop()
        
        # 创建界面
        self.create_widgets()
        
        # 绑定全局快捷键
        self.bind_shortcuts()
        
    def setup_drag_drop(self):
        """设置拖放功能"""
        try:
            # 尝试使用tkinterdnd2库实现拖放
            import tkinterdnd2
            self.root = tkinterdnd2.Tk()
            self.root.title("Map文件中断向量表分析器 - 支持对比")
            self.root.geometry("1000x700")
            self.root.drop_target_register('DND_Files')
            self.root.dnd_bind('<<Drop>>', self.on_drop)
            self.use_dnd = True
        except ImportError:
            self.use_dnd = False
            print("提示: 安装tkinterdnd2可以获得拖放支持")
            print("pip install tkinterdnd2")
    
    def on_drop(self, event):
        """处理拖放事件"""
        if self.use_dnd:
            files = event.data
            if isinstance(files, str):
                files = [files]
            if files:
                # 检查拖入的是文件夹还是文件
                path = files[0]
                if os.path.isdir(path):
                    self.process_folder(path)
                elif os.path.isfile(path) and path.lower().endswith('.map'):
                    self.add_map_file(path)
                else:
                    messagebox.showinfo("提示", "请拖入工程文件夹或.map文件")
    
    def bind_shortcuts(self):
        """绑定快捷键"""
        # Ctrl+F 搜索
        self.root.bind('<Control-f>', self.show_search_dialog)
        self.root.bind('<Control-F>', self.show_search_dialog)
        
        # Ctrl+N 下一个匹配
        self.root.bind('<Control-n>', self.find_next)
        self.root.bind('<Control-N>', self.find_next)
        
        # Ctrl+Shift+N 上一个匹配
        self.root.bind('<Control-Shift-N>', self.find_previous)
        
        # Ctrl+A 全选
        self.root.bind('<Control-a>', self.select_all)
        self.root.bind('<Control-A>', self.select_all)
    
    def get_active_text_widget(self):
        """获取当前活动的文本控件"""
        # 获取当前选中的笔记本页
        current_tab = self.notebook.select()
        if not current_tab:
            return None
        
        # 获取当前页的文本控件
        tab_name = self.notebook.tab(current_tab, "text")
        if tab_name == "中断向量表":
            return self.vector_text
        elif tab_name == "Map文件内容":
            return self.map_text
        elif tab_name == "对比结果":
            return self.compare_text
        return None
    
    def show_search_dialog(self, event=None):
        """显示搜索对话框"""
        text_widget = self.get_active_text_widget()
        if not text_widget:
            messagebox.showinfo("提示", "请先选择要搜索的内容页面")
            return
        
        # 如果已经有搜索窗口，关闭它
        if text_widget in self.search_windows:
            self.search_windows[text_widget].destroy()
            del self.search_windows[text_widget]
        
        # 创建搜索窗口
        search_win = tk.Toplevel(self.root)
        search_win.title("搜索")
        search_win.geometry("400x120")
        search_win.transient(self.root)
        search_win.grab_set()
        
        # 保存搜索窗口引用
        self.search_windows[text_widget] = search_win
        
        # 搜索框
        ttk.Label(search_win, text="搜索关键词:").pack(pady=(10, 0))
        search_var = tk.StringVar()
        search_entry = ttk.Entry(search_win, textvariable=search_var, width=40)
        search_entry.pack(pady=5)
        search_entry.focus_set()
        
        # 搜索选项
        option_frame = ttk.Frame(search_win)
        option_frame.pack(pady=5)
        
        case_sensitive = tk.BooleanVar(value=False)
        ttk.Checkbutton(option_frame, text="区分大小写", 
                       variable=case_sensitive).pack(side=tk.LEFT, padx=5)
        
        # 按钮框架
        btn_frame = ttk.Frame(search_win)
        btn_frame.pack(pady=5)
        
        def do_search():
            keyword = search_var.get()
            if not keyword:
                return
            
            # 清除之前的选中
            text_widget.tag_remove('search_highlight', '1.0', tk.END)
            
            # 搜索并高亮
            count = self.highlight_search_results(text_widget, keyword, case_sensitive.get())
            
            if count > 0:
                # 跳到第一个结果
                self.goto_next_match(text_widget)
                search_win.title(f"搜索 - 找到 {count} 个匹配")
            else:
                messagebox.showinfo("搜索", f"未找到关键词: {keyword}")
                search_win.title("搜索 - 未找到")
        
        def on_enter(event):
            do_search()
        
        search_entry.bind('<Return>', on_enter)
        
        ttk.Button(btn_frame, text="查找", command=do_search).pack(side=tk.LEFT, padx=5)
        ttk.Button(btn_frame, text="关闭", command=search_win.destroy).pack(side=tk.LEFT, padx=5)
        
        # 窗口关闭时清理
        def on_close():
            if text_widget in self.search_windows:
                del self.search_windows[text_widget]
            search_win.destroy()
        
        search_win.protocol("WM_DELETE_WINDOW", on_close)
    
    def highlight_search_results(self, text_widget, keyword, case_sensitive=False):
        """高亮搜索结果显示"""
        # 清除之前的高亮
        text_widget.tag_remove('search_highlight', '1.0', tk.END)
        text_widget.tag_remove('search_current', '1.0', tk.END)
        
        # 配置高亮标签
        text_widget.tag_configure('search_highlight', background='yellow', foreground='black')
        text_widget.tag_configure('search_current', background='orange', foreground='black')
        
        # 搜索
        count = 0
        start_pos = '1.0'
        
        if not case_sensitive:
            # 不区分大小写搜索
            content = text_widget.get('1.0', tk.END)
            keyword_lower = keyword.lower()
            content_lower = content.lower()
            
            # 使用字符串查找来获取所有位置
            pos = 0
            while True:
                pos = content_lower.find(keyword_lower, pos)
                if pos == -1:
                    break
                
                # 计算行号和列号
                # 找到这个位置所在的行
                lines = content[:pos].split('\n')
                line_num = len(lines)
                col_num = len(lines[-1])
                
                start_idx = f"{line_num}.{col_num}"
                end_idx = f"{line_num}.{col_num + len(keyword)}"
                
                text_widget.tag_add('search_highlight', start_idx, end_idx)
                count += 1
                pos += 1
        else:
            # 区分大小写搜索
            start_pos = '1.0'
            while True:
                start_pos = text_widget.search(keyword, start_pos, tk.END, 
                                              nocase=False, exact=True)
                if not start_pos:
                    break
                
                end_pos = f"{start_pos}+{len(keyword)}c"
                text_widget.tag_add('search_highlight', start_pos, end_pos)
                count += 1
                start_pos = end_pos
        
        return count
    
    def goto_next_match(self, text_widget):
        """跳转到下一个匹配"""
        # 找到当前选中的位置
        current_pos = None
        if text_widget.tag_ranges('search_current'):
            current_pos = text_widget.tag_ranges('search_current')[0]
        
        # 获取所有高亮位置
        highlight_ranges = text_widget.tag_ranges('search_highlight')
        if not highlight_ranges:
            return False
        
        # 清除当前高亮
        text_widget.tag_remove('search_current', '1.0', tk.END)
        
        # 找到下一个匹配
        if current_pos:
            found = False
            for i in range(0, len(highlight_ranges), 2):
                if highlight_ranges[i] > current_pos or highlight_ranges[i] == current_pos:
                    text_widget.tag_add('search_current', highlight_ranges[i], highlight_ranges[i+1])
                    text_widget.see(highlight_ranges[i])
                    found = True
                    break
            
            if not found and len(highlight_ranges) >= 2:
                # 回到第一个
                text_widget.tag_add('search_current', highlight_ranges[0], highlight_ranges[1])
                text_widget.see(highlight_ranges[0])
        else:
            # 从第一个开始
            if len(highlight_ranges) >= 2:
                text_widget.tag_add('search_current', highlight_ranges[0], highlight_ranges[1])
                text_widget.see(highlight_ranges[0])
        
        return True
    
    def find_next(self, event=None):
        """查找下一个匹配"""
        text_widget = self.get_active_text_widget()
        if not text_widget:
            return
        
        # 检查是否有高亮结果
        if text_widget.tag_ranges('search_highlight'):
            self.goto_next_match(text_widget)
        else:
            # 如果没有搜索结果，显示搜索对话框
            self.show_search_dialog()
    
    def find_previous(self, event=None):
        """查找上一个匹配"""
        text_widget = self.get_active_text_widget()
        if not text_widget:
            return
        
        # 获取所有高亮位置
        highlight_ranges = text_widget.tag_ranges('search_highlight')
        if not highlight_ranges:
            self.show_search_dialog()
            return
        
        # 获取当前选中的位置
        current_pos = None
        if text_widget.tag_ranges('search_current'):
            current_pos = text_widget.tag_ranges('search_current')[0]
        
        # 清除当前高亮
        text_widget.tag_remove('search_current', '1.0', tk.END)
        
        # 找到上一个匹配
        if current_pos:
            found = False
            for i in range(len(highlight_ranges)-2, -1, -2):
                if highlight_ranges[i] < current_pos:
                    text_widget.tag_add('search_current', highlight_ranges[i], highlight_ranges[i+1])
                    text_widget.see(highlight_ranges[i])
                    found = True
                    break
            
            if not found and len(highlight_ranges) >= 2:
                # 回到最后一个
                last_idx = len(highlight_ranges) - 2
                text_widget.tag_add('search_current', highlight_ranges[last_idx], highlight_ranges[last_idx+1])
                text_widget.see(highlight_ranges[last_idx])
        else:
            # 从最后一个开始
            if len(highlight_ranges) >= 2:
                last_idx = len(highlight_ranges) - 2
                text_widget.tag_add('search_current', highlight_ranges[last_idx], highlight_ranges[last_idx+1])
                text_widget.see(highlight_ranges[last_idx])
    
    def select_all(self, event=None):
        """全选当前文本"""
        text_widget = self.get_active_text_widget()
        if text_widget:
            text_widget.tag_add(tk.SEL, '1.0', tk.END)
            text_widget.mark_set(tk.INSERT, '1.0')
            text_widget.see(tk.INSERT)
            return 'break'
    
    def create_widgets(self):
        """创建界面组件"""
        # 主框架
        main_frame = ttk.Frame(self.root, padding="10")
        main_frame.pack(fill=tk.BOTH, expand=True)
        
        # 顶部控制区域
        control_frame = ttk.Frame(main_frame)
        control_frame.pack(fill=tk.X, pady=5)
        
        # 标题
        title_label = ttk.Label(control_frame, text="Map文件中断向量表分析器", 
                                font=('Arial', 14, 'bold'))
        title_label.pack(side=tk.LEFT, padx=5)
        
        # 快捷键提示
        shortcut_label = ttk.Label(control_frame, text="Ctrl+F搜索 | Ctrl+N下一个 | Ctrl+Shift+N上一个", 
                                   font=('Arial', 9), foreground='gray')
        shortcut_label.pack(side=tk.LEFT, padx=20)
        
        # 按钮区域
        btn_frame = ttk.Frame(control_frame)
        btn_frame.pack(side=tk.RIGHT)
        
        add_btn = ttk.Button(btn_frame, text="添加Map文件", 
                            command=self.add_map_file_dialog)
        add_btn.pack(side=tk.LEFT, padx=2)
        
        folder_btn = ttk.Button(btn_frame, text="添加工程文件夹", 
                               command=self.select_folder)
        folder_btn.pack(side=tk.LEFT, padx=2)
        
        clear_btn = ttk.Button(btn_frame, text="清除所有", 
                              command=self.clear_all_maps)
        clear_btn.pack(side=tk.LEFT, padx=2)
        
        # 新增导出按钮
        export_btn = ttk.Button(btn_frame, text="导出内存分布", 
                               command=self.export_memory_distribution)
        export_btn.pack(side=tk.LEFT, padx=2)
        
        # 拖放提示
        if self.use_dnd:
            drop_label = ttk.Label(main_frame, text="📁 拖放文件夹或.map文件到这里", 
                                   font=('Arial', 10), foreground='gray')
            drop_label.pack(pady=2)
        
        # 创建左右分栏
        paned_window = ttk.PanedWindow(main_frame, orient=tk.HORIZONTAL)
        paned_window.pack(fill=tk.BOTH, expand=True, pady=5)
        
        # 左侧：文件列表
        left_frame = ttk.Frame(paned_window)
        paned_window.add(left_frame, weight=1)
        
        # 文件列表标题
        list_label = ttk.Label(left_frame, text="已加载的Map文件", 
                              font=('Arial', 10, 'bold'))
        list_label.pack(pady=5)
        
        # 文件列表
        list_frame = ttk.Frame(left_frame)
        list_frame.pack(fill=tk.BOTH, expand=True, padx=5)
        
        list_scroll = ttk.Scrollbar(list_frame)
        list_scroll.pack(side=tk.RIGHT, fill=tk.Y)
        
        self.map_listbox = tk.Listbox(list_frame, yscrollcommand=list_scroll.set,
                                      selectmode=tk.SINGLE, height=10)
        self.map_listbox.pack(fill=tk.BOTH, expand=True)
        list_scroll.config(command=self.map_listbox.yview)
        self.map_listbox.bind('<<ListboxSelect>>', self.on_map_select)
        
        # 操作按钮
        list_btn_frame = ttk.Frame(left_frame)
        list_btn_frame.pack(fill=tk.X, pady=5)
        
        remove_btn = ttk.Button(list_btn_frame, text="移除选中", 
                               command=self.remove_selected_map)
        remove_btn.pack(side=tk.LEFT, padx=2)
        
        compare_btn = ttk.Button(list_btn_frame, text="对比模式", 
                                command=self.show_comparison)
        compare_btn.pack(side=tk.LEFT, padx=2)
        
        # 右侧：显示区域
        right_frame = ttk.Frame(paned_window)
        paned_window.add(right_frame, weight=3)
        
        # 创建分页笔记本
        self.notebook = ttk.Notebook(right_frame)
        self.notebook.pack(fill=tk.BOTH, expand=True)
        
        # 中断向量表页
        vector_frame = ttk.Frame(self.notebook)
        self.notebook.add(vector_frame, text="中断向量表")
        
        vector_scroll = ttk.Scrollbar(vector_frame)
        vector_scroll.pack(side=tk.RIGHT, fill=tk.Y)
        
        self.vector_text = tk.Text(vector_frame, yscrollcommand=vector_scroll.set,
                                   wrap=tk.NONE, font=('Courier New', 9))
        self.vector_text.pack(fill=tk.BOTH, expand=True)
        vector_scroll.config(command=self.vector_text.yview)
        
        # Map文件内容页
        map_frame = ttk.Frame(self.notebook)
        self.notebook.add(map_frame, text="Map文件内容")
        
        map_scroll = ttk.Scrollbar(map_frame)
        map_scroll.pack(side=tk.RIGHT, fill=tk.Y)
        
        self.map_text = tk.Text(map_frame, yscrollcommand=map_scroll.set,
                                wrap=tk.WORD, font=('Courier New', 9))
        self.map_text.pack(fill=tk.BOTH, expand=True)
        map_scroll.config(command=self.map_text.yview)
        
        # 对比结果页
        compare_frame = ttk.Frame(self.notebook)
        self.notebook.add(compare_frame, text="对比结果")
        
        compare_scroll = ttk.Scrollbar(compare_frame)
        compare_scroll.pack(side=tk.RIGHT, fill=tk.Y)
        
        self.compare_text = tk.Text(compare_frame, yscrollcommand=compare_scroll.set,
                                    wrap=tk.WORD, font=('Courier New', 9))
        self.compare_text.pack(fill=tk.BOTH, expand=True)
        compare_scroll.config(command=self.compare_text.yview)
        
        # 状态栏
        self.status_var = tk.StringVar(value="就绪 - 请添加.map文件或工程文件夹")
        status_bar = ttk.Label(main_frame, textvariable=self.status_var, 
                              relief=tk.SUNKEN, anchor=tk.W)
        status_bar.pack(fill=tk.X, pady=5)
        
        self.current_folder = None
    
    def export_memory_distribution(self):
        """导出内存分布到txt文件"""
        if not self.loaded_maps:
            messagebox.showwarning("警告", "没有加载任何map文件，请先添加map文件")
            return
        
        # 让用户选择保存位置
        file_path = filedialog.asksaveasfilename(
            title="保存内存分布文件",
            defaultextension=".txt",
            filetypes=[("文本文件", "*.txt"), ("所有文件", "*.*")],
            initialfile="内存分布map.txt"
        )
        
        if not file_path:
            return  # 用户取消
        
        try:
            # 构建导出内容
            export_content = []
            export_content.append("=" * 80)
            export_content.append("中断向量表内存分布导出")
            export_content.append(f"导出时间: {datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
            export_content.append(f"包含文件数: {len(self.loaded_maps)}")
            export_content.append("=" * 80)
            export_content.append("")
            
            # 遍历所有加载的map文件
            for file_path_key, info in self.loaded_maps.items():
                display_name = info['display_name']
                vectors = info['vectors']
                
                export_content.append(f"\n{'=' * 80}")
                export_content.append(f"文件: {display_name}")
                export_content.append(f"完整路径: {file_path_key}")
                export_content.append(f"中断向量数量: {len(vectors)}")
                export_content.append(f"{'=' * 80}")
                export_content.append("")
                
                if vectors:
                    # 按地址排序
                    sorted_vectors = sorted(vectors, key=lambda x: int(x[0], 16) if x[0] else 0)
                    
                    # 添加表头
                    export_content.append(f"{'地址':<15} {'名称':<40} {'类型':<30}")
                    export_content.append("-" * 85)
                    
                    # 添加每个向量
                    for addr, name, desc in sorted_vectors:
                        addr_str = f"0x{addr.upper():>8}" if isinstance(addr, str) and len(addr) <= 8 else f"0x{addr.upper()}"
                        export_content.append(f"{addr_str:<15} {name:<40} {desc:<30}")
                    
                    export_content.append("")
                    export_content.append(f"总计: {len(vectors)} 个中断向量")
                else:
                    export_content.append("未找到明显的中断向量信息")
                
                export_content.append("")
            
            # 添加汇总信息
            export_content.append("\n" + "=" * 80)
            export_content.append("汇总统计")
            export_content.append("=" * 80)
            
            # 统计所有向量地址
            all_addresses = {}
            for file_path_key, info in self.loaded_maps.items():
                display_name = info['display_name']
                for addr, name, desc in info['vectors']:
                    if addr not in all_addresses:
                        all_addresses[addr] = []
                    all_addresses[addr].append((display_name, name, desc))
            
            export_content.append(f"\n所有文件中出现的中断向量地址总数: {len(all_addresses)}")
            
            # 显示出现次数最多的地址
            addr_counts = [(addr, len(info)) for addr, info in all_addresses.items()]
            addr_counts.sort(key=lambda x: x[1], reverse=True)
            
            export_content.append("\n出现频率最高的地址:")
            for addr, count in addr_counts[:10]:  # 显示前10个
                addr_str = f"0x{addr.upper():>8}" if isinstance(addr, str) and len(addr) <= 8 else f"0x{addr.upper()}"
                export_content.append(f"  {addr_str}: 出现 {count} 次")
            
            # 写入文件
            with open(file_path, 'w', encoding='utf-8') as f:
                f.write('\n'.join(export_content))
            
            self.status_var.set(f"内存分布已导出到: {os.path.basename(file_path)}")
            messagebox.showinfo("导出成功", f"内存分布已成功导出到:\n{file_path}")
            
        except Exception as e:
            messagebox.showerror("导出失败", f"导出过程中发生错误:\n{str(e)}")
            self.status_var.set(f"导出失败: {str(e)}")
        
    def add_map_file_dialog(self):
        """通过对话框添加.map文件"""
        file_paths = filedialog.askopenfilenames(
            title="选择Map文件",
            filetypes=[("Map文件", "*.map"), ("所有文件", "*.*")]
        )
        for file_path in file_paths:
            self.add_map_file(file_path)
    
    def add_map_file(self, file_path):
        """添加单个.map文件"""
        if not os.path.exists(file_path):
            messagebox.showerror("错误", f"文件不存在: {file_path}")
            return
        
        if file_path in self.loaded_maps:
            messagebox.showinfo("提示", f"文件已加载: {os.path.basename(file_path)}")
            return
        
        self.status_var.set(f"正在加载: {os.path.basename(file_path)}")
        
        try:
            # 读取文件内容
            with open(file_path, 'r', encoding='utf-8', errors='ignore') as f:
                content = f.read()
            
            # 分析向量信息
            vectors = self.extract_vectors(content)
            
            # 存储信息
            display_name = os.path.basename(file_path)
            self.loaded_maps[file_path] = {
                'content': content,
                'vectors': vectors,
                'display_name': display_name,
                'file_path': file_path
            }
            
            # 更新列表
            self.map_listbox.insert(tk.END, display_name)
            self.map_listbox.selection_set(tk.END)
            
            # 自动选择新加载的文件
            self.current_selection = file_path
            self.display_map_info(file_path)
            
            self.status_var.set(f"已加载: {display_name} (共{len(self.loaded_maps)}个文件)")
            
        except Exception as e:
            messagebox.showerror("错误", f"加载文件失败: {str(e)}")
            self.status_var.set(f"加载失败: {str(e)}")
    
    def select_folder(self):
        """选择文件夹"""
        folder = filedialog.askdirectory(title="选择工程文件夹")
        if folder:
            self.process_folder(folder)
    
    def process_folder(self, folder_path):
        """处理选择的文件夹"""
        self.current_folder = folder_path
        self.status_var.set(f"正在扫描文件夹: {folder_path}")
        
        try:
            # 查找所有.map文件
            map_files = list(Path(folder_path).rglob("*.map"))
            
            if not map_files:
                messagebox.showwarning("警告", f"在文件夹中未找到.map文件！\n{folder_path}")
                self.status_var.set("未找到.map文件")
                return
            
            # 添加所有找到的map文件
            added_count = 0
            for map_file in map_files:
                if str(map_file) not in self.loaded_maps:
                    self.add_map_file(str(map_file))
                    added_count += 1
            
            if added_count == 0:
                messagebox.showinfo("提示", "所有map文件已加载")
            else:
                messagebox.showinfo("成功", f"成功添加 {added_count} 个map文件")
            
            self.status_var.set(f"扫描完成，添加了 {added_count} 个文件")
            
        except Exception as e:
            messagebox.showerror("错误", f"处理文件夹时出错：{str(e)}")
            self.status_var.set(f"错误: {str(e)}")
    
    def clear_all_maps(self):
        """清除所有加载的map文件"""
        if self.loaded_maps:
            if messagebox.askyesno("确认", "确定要清除所有已加载的map文件吗？"):
                self.loaded_maps.clear()
                self.map_listbox.delete(0, tk.END)
                self.vector_text.delete(1.0, tk.END)
                self.map_text.delete(1.0, tk.END)
                self.compare_text.delete(1.0, tk.END)
                self.status_var.set("已清除所有文件")
    
    def remove_selected_map(self):
        """移除选中的map文件"""
        selection = self.map_listbox.curselection()
        if not selection:
            messagebox.showwarning("警告", "请先选择一个文件")
            return
        
        index = selection[0]
        display_name = self.map_listbox.get(index)
        
        # 找到对应的文件路径
        for file_path, info in self.loaded_maps.items():
            if info['display_name'] == display_name:
                del self.loaded_maps[file_path]
                break
        
        self.map_listbox.delete(index)
        
        # 清空显示
        self.vector_text.delete(1.0, tk.END)
        self.map_text.delete(1.0, tk.END)
        
        # 如果有其他文件，自动选择第一个
        if self.loaded_maps:
            self.map_listbox.selection_set(0)
            first_file = list(self.loaded_maps.keys())[0]
            self.display_map_info(first_file)
        
        self.status_var.set(f"已移除: {display_name}")
    
    def on_map_select(self, event):
        """处理列表选择事件"""
        selection = self.map_listbox.curselection()
        if selection:
            index = selection[0]
            display_name = self.map_listbox.get(index)
            
            # 找到对应的文件路径
            for file_path, info in self.loaded_maps.items():
                if info['display_name'] == display_name:
                    self.current_selection = file_path
                    self.display_map_info(file_path)
                    break
    
    def display_map_info(self, file_path):
        """显示选中的map文件信息"""
        if file_path not in self.loaded_maps:
            return
        
        info = self.loaded_maps[file_path]
        
        # 清空显示
        self.vector_text.delete(1.0, tk.END)
        self.map_text.delete(1.0, tk.END)
        
        # 显示向量信息
        self.display_vectors(info['vectors'], os.path.basename(file_path))
        
        # 显示map文件内容
        self.map_text.insert(tk.END, f"Map文件: {file_path}\n")
        self.map_text.insert(tk.END, "=" * 80 + "\n\n")
        self.map_text.insert(tk.END, info['content'][:100000])  # 显示前100000字符
        
        self.status_var.set(f"显示: {os.path.basename(file_path)}")
    
    def display_vectors(self, vectors, filename):
        """显示向量表信息"""
        self.vector_text.insert(tk.END, f"中断向量表分析 - {filename}\n")
        self.vector_text.insert(tk.END, "=" * 80 + "\n\n")
        
        if vectors:
            self.vector_text.insert(tk.END, f"找到 {len(vectors)} 个中断向量:\n\n")
            
            # 按地址排序
            vectors.sort(key=lambda x: int(x[0], 16) if x[0] else 0)
            
            # 创建表格
            self.vector_text.insert(tk.END, f"{'地址':<12} {'名称':<30} {'类型':<20}\n")
            self.vector_text.insert(tk.END, "-" * 70 + "\n")
            
            for addr, name, desc in vectors:
                if addr:
                    addr_str = f"0x{addr.upper():>8}" if isinstance(addr, str) and len(addr) <= 8 else f"0x{addr.upper()}"
                    self.vector_text.insert(tk.END, f"{addr_str:<12} {name:<30} {desc:<20}\n")
            
            # 统计信息
            self.vector_text.insert(tk.END, "\n" + "-" * 70 + "\n")
            self.vector_text.insert(tk.END, f"总计: {len(vectors)} 个中断向量\n")
        else:
            self.vector_text.insert(tk.END, "未找到明显的中断向量信息\n")
    
    def extract_vectors(self, content):
        """提取中断向量信息"""
        vectors = []
        
        patterns = [
            (r'(\w+_IRQHandler|IRQ\d+)\s+0x([0-9A-Fa-f]+)', "中断处理函数"),
            (r'__Vectors\s+0x([0-9A-Fa-f]+)', "向量表起始"),
            (r'Vector\s+(\d+)\s+0x([0-9A-Fa-f]+)', "向量编号"),
            (r'(\w+)\s+0x([0-9A-Fa-f]+)\s+.*?vector', "向量表项"),
            (r'(\w+_IRQn|IRQ\d+)\s+=\s+0x([0-9A-Fa-f]+)', "中断编号"),
            (r'(.*?handler|.*?ISR)\s+0x([0-9A-Fa-f]+)', "中断服务函数"),
            (r'0x([0-9A-Fa-f]+)\s+(\w+_IRQHandler)', "处理函数地址"),
            (r'0x([0-9A-Fa-f]+)\s+(\w+_ISR)', "ISR地址"),
        ]
        
        for pattern, desc in patterns:
            matches = re.findall(pattern, content, re.IGNORECASE)
            for match in matches:
                if len(match) == 2:
                    if re.match(r'^[0-9A-Fa-f]+$', match[0]) and len(match[0]) >= 4:
                        addr, name = match
                        vectors.append((addr, name, desc))
                    elif re.match(r'^[0-9A-Fa-f]+$', match[1]) and len(match[1]) >= 4:
                        name, addr = match
                        vectors.append((addr, name, desc))
        
        # 去重
        unique_vectors = []
        seen = set()
        for addr, name, desc in vectors:
            key = (addr, name)
            if key not in seen:
                seen.add(key)
                unique_vectors.append((addr, name, desc))
        
        return unique_vectors
    
    def show_comparison(self):
        """显示对比结果"""
        if len(self.loaded_maps) < 2:
            messagebox.showinfo("提示", "至少需要2个map文件才能进行对比")
            return
        
        self.compare_text.delete(1.0, tk.END)
        self.compare_text.insert(tk.END, "中断向量表对比\n")
        self.compare_text.insert(tk.END, "=" * 80 + "\n\n")
        
        # 收集所有文件的中断向量
        all_vectors = {}
        for file_path, info in self.loaded_maps.items():
            display_name = info['display_name']
            vectors = info['vectors']
            # 按地址建立索引
            vector_dict = {}
            for addr, name, desc in vectors:
                if addr not in vector_dict:
                    vector_dict[addr] = []
                vector_dict[addr].append((name, desc))
            all_vectors[display_name] = vector_dict
        
        # 收集所有地址
        all_addresses = set()
        for vector_dict in all_vectors.values():
            all_addresses.update(vector_dict.keys())
        
        # 排序地址
        sorted_addresses = sorted(all_addresses, key=lambda x: int(x, 16) if x else 0)
        
        # 显示对比表格
        if sorted_addresses:
            # 表头
            header = f"{'地址':<12}"
            for display_name in all_vectors.keys():
                header += f" {display_name:<30}"
            self.compare_text.insert(tk.END, header + "\n")
            self.compare_text.insert(tk.END, "-" * (12 + 31 * len(all_vectors)) + "\n")
            
            # 显示每个地址的信息
            for addr in sorted_addresses[:500]:  # 限制显示数量
                row = f"{addr.upper():<12}"
                for display_name, vector_dict in all_vectors.items():
                    if addr in vector_dict:
                        # 显示该地址下的所有名称
                        names = [name for name, desc in vector_dict[addr]]
                        row += f" {','.join(names):<30}"
                    else:
                        row += f" {'---':<30}"
                self.compare_text.insert(tk.END, row + "\n")
            
            self.compare_text.insert(tk.END, "\n" + "-" * 80 + "\n")
            self.compare_text.insert(tk.END, f"对比统计:\n")
            self.compare_text.insert(tk.END, f"- 总共对比地址数: {len(sorted_addresses)}\n")
            self.compare_text.insert(tk.END, f"- 参与对比的文件数: {len(all_vectors)}\n")
            
            # 显示每个文件的向量数量
            for display_name, vector_dict in all_vectors.items():
                self.compare_text.insert(tk.END, f"- {display_name}: {len(vector_dict)} 个向量\n")
            
            # 查找不同
            self.find_differences(all_vectors, sorted_addresses)
        else:
            self.compare_text.insert(tk.END, "没有找到可用于对比的中断向量信息\n")
        
        # 切换到对比页
        self.notebook.select(2)  # 对比结果页是索引2
        
        self.status_var.set("对比完成")
    
    def find_differences(self, all_vectors, sorted_addresses):
        """查找不同文件之间的差异"""
        self.compare_text.insert(tk.END, "\n" + "=" * 80 + "\n")
        self.compare_text.insert(tk.END, "差异分析:\n\n")
        
        file_names = list(all_vectors.keys())
        
        if len(file_names) >= 2:
            # 比较第一和第二个文件
            file1, file2 = file_names[0], file_names[1]
            vec1 = all_vectors[file1]
            vec2 = all_vectors[file2]
            
            # 只在file1中存在的地址
            only_in_1 = set(vec1.keys()) - set(vec2.keys())
            if only_in_1:
                self.compare_text.insert(tk.END, f"只在 {file1} 中存在:\n")
                for addr in sorted(only_in_1, key=lambda x: int(x, 16)):
                    names = [name for name, desc in vec1[addr]]
                    self.compare_text.insert(tk.END, f"  {addr}: {', '.join(names)}\n")
                self.compare_text.insert(tk.END, "\n")
            
            # 只在file2中存在的地址
            only_in_2 = set(vec2.keys()) - set(vec1.keys())
            if only_in_2:
                self.compare_text.insert(tk.END, f"只在 {file2} 中存在:\n")
                for addr in sorted(only_in_2, key=lambda x: int(x, 16)):
                    names = [name for name, desc in vec2[addr]]
                    self.compare_text.insert(tk.END, f"  {addr}: {', '.join(names)}\n")
                self.compare_text.insert(tk.END, "\n")
            
            # 共同的地址但名称不同
            common = set(vec1.keys()) & set(vec2.keys())
            different_names = []
            for addr in common:
                names1 = [name for name, desc in vec1[addr]]
                names2 = [name for name, desc in vec2[addr]]
                if set(names1) != set(names2):
                    different_names.append((addr, names1, names2))
            
            if different_names:
                self.compare_text.insert(tk.END, "相同地址但名称不同:\n")
                for addr, names1, names2 in different_names:
                    self.compare_text.insert(tk.END, f"  {addr}:\n")
                    self.compare_text.insert(tk.END, f"    {file1}: {', '.join(names1)}\n")
                    self.compare_text.insert(tk.END, f"    {file2}: {', '.join(names2)}\n")
                self.compare_text.insert(tk.END, "\n")

def main():
    try:
        # 尝试使用tkinterdnd2
        try:
            import tkinterdnd2
            root = tkinterdnd2.Tk()
        except ImportError:
            root = tk.Tk()
        
        app = MapFileAnalyzer(root)
        
        def on_closing():
            root.destroy()
        
        root.protocol("WM_DELETE_WINDOW", on_closing)
        root.mainloop()
        
    except Exception as e:
        print(f"程序启动失败: {e}")
        input("按Enter键退出...")

if __name__ == "__main__":
    main()
