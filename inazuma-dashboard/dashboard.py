# ==============================================================================
# Copyright (c) 2026 Nexus_Yao. All rights reserved.
#
# Project:      Inazuma OS
# File:         app.py
# Description:  AI 网关与大模型生态系统的管理后台。
#               基于 Streamlit 构建，提供网关实时监控面板、模型并发调用统计、
#               以及底层租户鉴权 Token 与调用配额的管控界面。
# ==============================================================================

import streamlit as st
import pymysql
import pandas as pd
import plotly.express as px
import secrets
import socket
import json
from datetime import datetime

# =========================================================================
# 全局环境配置与视图样式初始化
# =========================================================================
st.set_page_config(page_title="Inazuma OS", page_icon="⚡", layout="wide")

# 注入自定义 CSS 以标准化图表组件的展示层级与阴影过渡效果
st.markdown("""
<style>
    [data-testid="stGraphWidget"] {
        background-color: #ffffff;
        border-radius: 16px;
        box-shadow: 0px 12px 30px -5px rgba(123, 44, 191, 0.3);
        padding: 15px;
        transition: transform 0.3s ease;
    }
    [data-testid="stGraphWidget"]:hover {
        transform: translateY(-5px);
        box-shadow: 0px 18px 35px -5px rgba(123, 44, 191, 0.4);
    }
</style>
""", unsafe_allow_html=True)

# 定义图表渲染的调色板常量
PURPLE_COLORS = ['#7B2CBF', '#9D4EDD', '#C77DFF', '#E0AAFF', '#5A189A']
STANDBY_COLORS = ['#EAEAEA', '#F5F5F5', '#FAFAFA'] 

# 定义 Plotly 图表的全局轻量化布局配置
light_layout = dict(
    plot_bgcolor='white',
    paper_bgcolor='white',
    margin=dict(t=50, b=40, l=20, r=20),
    uirevision='constant', 
    font=dict(color='#333333'),
)

# =========================================================================
# 数据持久化与连接管理
# =========================================================================
# 创建并返回 MySQL 数据库连接实例 (字典游标模式)
def get_mysql_conn():
    return pymysql.connect(
        host='127.0.0.1', user='inazuma_user', password='Inazuma_2026!',
        database='inazuma_chat', cursorclass=pymysql.cursors.DictCursor, connect_timeout=2
    )

# =========================================================================
# 服务端状态探针与指标采集
# =========================================================================
# 通过 Socket 发起 HTTP GET 请求，拉取底层 io_uring 网关的内存指标。
# 包含自动重试与超时控制，以防止在网关高负载或冷启动状态下阻塞前端渲染线程。
def get_gateway_metrics():
    for attempt in range(2):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(1.0)
            s.connect(('127.0.0.1', 8085))
            
            req = "GET /metrics HTTP/1.1\r\nHost: 127.0.0.1:8085\r\nConnection: close\r\n\r\n"
            s.sendall(req.encode('utf-8'))
            
            resp = b""
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                resp += chunk
            s.close()
            
            resp_str = resp.decode('utf-8', errors='ignore')
            body_start = resp_str.find("\r\n\r\n")
            if body_start != -1:
                body = resp_str[body_start+4:]
                return json.loads(body), True
            return {}, False
        except Exception as e:
            if attempt == 1:
                return {"error": str(e)}, False

# =========================================================================
# 视图组件：侧边栏导航控制
# =========================================================================
st.sidebar.title("⚡ Inazuma 控制中心")
st.sidebar.markdown("---")
page = st.sidebar.radio("跳转页面", ["📈 全链路监控大屏", "🔑 Token 与用户管理"])
st.sidebar.markdown("---")
st.sidebar.caption(f"Current Date: {datetime.now().strftime('%Y-%m-%d')}")
st.sidebar.caption("Operator: Yaozheng")

# =========================================================================
# 视图组件：网关全链路监控大屏
# =========================================================================
# 利用 st.fragment 装饰器实现组件级独立刷新 (2秒间隔)
@st.fragment(run_every=2)
def render_dashboard():
    metrics, gw_alive = get_gateway_metrics()
    
    if not gw_alive: 
        st.error(f"🔴 无法连接 Inazuma Gateway，监控探针已断开 | ERROR: {metrics.get('error', '未知')}")

    # 解析流量与命中率指标
    hits = metrics.get("hits", 0)
    misses = metrics.get("misses", 0)
    total_requests = hits + misses
    hit_rate = (hits / total_requests * 100) if total_requests > 0 else 0.0
    
    col1, col2, col3, col4 = st.columns(4)
    
    col1.metric("请求总数 (Total)", f"{int(total_requests):,}")
    col2.metric("缓存命中 (Hits)", f"{int(hits):,}", delta=f"Saved ¥{hits*0.02:,.2f}")
    col3.metric("实时命中率", f"{hit_rate:.1f}%")
    
    status_text = "🟢 ONLINE / ACTIVE" if (gw_alive and total_requests > 0) else ("⚪ STANDBY" if gw_alive else "🔴 OFFLINE")
    col4.metric("网关状态", status_text)

    st.markdown("<br>", unsafe_allow_html=True)

    # 构建大模型并发调用分布图表数据
    models = ['qwen-turbo', 'gpt-4o', 'deepseek-chat']
    counts = [metrics.get(m, 0) for m in models]
    
    df = pd.DataFrame({"Model": models, "Calls": counts})
    total_calls_in_chart = sum(counts)

    col_chart1, col_chart2 = st.columns(2)
    
    with col_chart1:
        # 根据当前流量状态渲染饼图 (空载状态渲染灰色占位符)
        if total_calls_in_chart == 0:
            fig_pie = px.pie(names=models, values=[1,1,1], title="🔥 算力占比分发 (待机)", hole=0.75)
            fig_pie.update_traces(marker=dict(colors=STANDBY_COLORS, line=dict(width=0)), textinfo='none')
        else:
            fig_pie = px.pie(df, values='Calls', names='Model', title="🔥 算力占比分发 (实时)", hole=0.75, 
                             color_discrete_sequence=PURPLE_COLORS)
            
            # 高亮占比最大的数据块
            max_idx = counts.index(max(counts))
            pulls = [0.05 if i == max_idx else 0 for i in range(len(counts))]

            fig_pie.update_traces(
                pull=pulls,
                textinfo='label+value+percent', 
                texttemplate="<b>%{label}</b><br>%{value} 次<br>(%{percent})",
                textfont_size=13,
                marker=dict(line=dict(width=0)) 
            )
        
        fig_pie.update_layout(showlegend=False, **light_layout)
        st.plotly_chart(fig_pie, use_container_width=True)
        
    with col_chart2:
        # 渲染请求并发排行柱状图
        fig_bar = px.bar(df, x='Model', y='Calls', title="📈 模型并发调用排行", color='Model', 
                         color_discrete_sequence=PURPLE_COLORS)
        
        fig_bar.update_traces(
            texttemplate='<b>%{y} 次</b>', 
            textposition='outside', 
            cliponaxis=False,
            textfont_size=15,
            marker=dict(line=dict(width=0)),
            width=0.45 
        )
        
        # 动态调整 Y 轴量程范围以确保文本标签不被遮挡
        max_y = max(counts) * 1.3 if total_calls_in_chart > 0 else 10
        fig_bar.update_layout(
            yaxis_title="", 
            xaxis_title="",
            showlegend=False, 
            transition=dict(duration=500),
            **light_layout
        )
        fig_bar.update_yaxes(showticklabels=False, showgrid=False, range=[0, max_y])
        fig_bar.update_xaxes(showgrid=False, tickfont=dict(size=14, color='#555555'))
        
        st.plotly_chart(fig_bar, use_container_width=True)

# =========================================================================
# 视图组件：授权用户数据表
# =========================================================================
# 利用 st.fragment 实现较低频率的表格数据自动刷新
@st.fragment(run_every=1.5)
def render_user_table():
    st.subheader("📋 授权用户明细")
    try:
        db = get_mysql_conn()
        with db.cursor() as cursor:
            cursor.execute("SELECT id, username, quota, api_key, created_at FROM users LIMIT 10")
            user_data = cursor.fetchall()
        db.close()
        
        if user_data:
            st.dataframe(pd.DataFrame(user_data), use_container_width=True, hide_index=True)
        else:
            st.info("目前还没有用户，快去发一张卡吧！")
    except Exception as e:
        st.error(f"❌ MySQL连接失败: {e}")

# =========================================================================
# 主页面路由与业务逻辑流转
# =========================================================================
if page == "📈 全链路监控大屏":
    st.title("⚡ Inazuma Ecosystem 全链路监控大屏")
    render_dashboard()

elif page == "🔑 Token 与用户管理":
    st.title("🔑 Inazuma 权限管理后台")
    render_user_table()
    st.divider()
    col_user1, col_user2 = st.columns(2)

    # 业务模块：新建租户并签发 Token
    with col_user1:
        st.subheader("🚀 发放新 Token")
        with st.form("new_user_form", clear_on_submit=True):
            new_username = st.text_input("用户名", placeholder="例如：Student_01")
            init_quota = st.number_input("初始配额", min_value=1000, value=100000, step=10000)
            
            if st.form_submit_button("✨ 确认开户") and new_username:
                try:
                    # 使用标准库生成高强度随机 Token
                    new_token = f"sk-inazuma-{secrets.token_hex(8)}"
                    db = get_mysql_conn()
                    with db.cursor() as cursor:
                        cursor.execute("INSERT INTO users (username, api_key, quota, password_hash) VALUES (%s, %s, %s, 'N/A')", 
                                       (new_username, new_token, init_quota))
                    db.commit()
                    db.close()
                    st.success("✅ 开户成功！")
                    st.code(f"用户名: {new_username}\nToken: {new_token}", language="bash")
                except Exception as e:
                    st.error(f"❌ 开户失败 (可能用户名已存在): {e}")

    # 业务模块：租户额度充值管理
    with col_user2:
        st.subheader("💳 后台充值")
        with st.form("topup_form", clear_on_submit=True):
            # 获取用户列表用于下拉框填充
            try:
                db = get_mysql_conn()
                with db.cursor() as cursor:
                    cursor.execute("SELECT username FROM users ORDER BY username ASC")
                    all_users = [row['username'] for row in cursor.fetchall()]
                db.close()
            except:
                all_users = []
                
            target_user = st.selectbox("选择充值用户", all_users) if all_users else None
            topup_amount = st.number_input("充值数量", min_value=1000, step=10000, value=50000)
            
            if st.form_submit_button("⚡ 立即到账") and target_user:
                try:
                    db = get_mysql_conn()
                    with db.cursor() as cursor:
                        cursor.execute("UPDATE users SET quota = quota + %s WHERE username = %s", (topup_amount, target_user))
                    db.commit()
                    db.close()
                    st.success(f"✅ 成功为 {target_user} 注入 {topup_amount:,} Tokens！")
                except Exception as e:
                    st.error(f"❌ 充值失败: {e}")

st.markdown("---")
st.caption("✨ Inazuma 2026 - Powered by io_uring & AF_XDP - High Performance AI Gateway")