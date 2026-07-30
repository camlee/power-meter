#include "power_screen.h"
#include "../../data/power_flow.h"
#include "../../sensors/sensors.h"
#include "../theme/ui_theme.h"
#include <cmath>

namespace power_screen {
namespace {
constexpr size_t kPoints = 240; // two minutes at the 500 ms sensor cadence
constexpr uint32_t kRefreshMs = 500; // match the Sensors screen and acquisition cadence
lv_obj_t* plot = nullptr;
lv_obj_t* kpiValues[4] = {};
float values[4][kPoints] = {};
sensors::Reading samples[sensors::SENSOR_COUNT][kPoints] = {};
size_t count = 0;
float minimum = -1, maximum = 1, step = 1;

float nice(float value) { const float p = powf(10, floorf(log10f(fmaxf(value, 1)))); const float n = value / p; return (n <= 1 ? 1 : n <= 2 ? 2 : n <= 5 ? 5 : 10) * p; }
int yFor(const lv_area_t& a, float value) { const int top = a.y1 + 18, bottom = a.y2 - 24; return top + lroundf((maximum - value) * (bottom - top) / (maximum - minimum)); }
void label(lv_draw_ctx_t* ctx, const char* text, int x, int y, lv_color_t color) { lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d); d.font=&lv_font_montserrat_14; d.color=color; lv_area_t area{(lv_coord_t)x,(lv_coord_t)y,(lv_coord_t)(x+90),(lv_coord_t)(y+16)}; lv_draw_label(ctx,&d,&area,text,nullptr); }
void drawCb(lv_event_t* e) {
 if(lv_event_get_code(e)!=LV_EVENT_DRAW_MAIN) return; auto* ctx=lv_event_get_draw_ctx(e); const auto& a=lv_event_get_target(e)->coords; const int left=a.x1+42,right=a.x2-4,top=a.y1+18,bottom=a.y2-24,w=right-left;
 lv_draw_line_dsc_t grid; lv_draw_line_dsc_init(&grid); grid.color=lv_palette_lighten(LV_PALETTE_GREY,2); grid.width=1;
 for(float tick=minimum; tick<=maximum+step*.1f; tick+=step){int y=yFor(a,tick); lv_point_t p1{(lv_coord_t)left,(lv_coord_t)y},p2{(lv_coord_t)right,(lv_coord_t)y}; grid.width=fabsf(tick)<.01f?2:1;lv_draw_line(ctx,&grid,&p1,&p2);char b[12];lv_snprintf(b,sizeof(b),"%d W",(int)lroundf(tick));label(ctx,b,a.x1+1,y-7,lv_palette_main(LV_PALETTE_GREY));}
 for(int t=0;t<=2;t++){int x=left+w*t/2;lv_point_t p1{(lv_coord_t)x,(lv_coord_t)top},p2{(lv_coord_t)x,(lv_coord_t)bottom};grid.width=1;lv_draw_line(ctx,&grid,&p1,&p2);label(ctx,t==2?"now":t==1?"-1m":"-2m",t==2?right-34:x-12,bottom+5,lv_palette_main(LV_PALETTE_GREY));}
 const lv_color_t colors[]={lv_color_hex(0x0000FF),lv_color_hex(0xFFA500),lv_color_hex(0x00BFFF),lv_color_hex(0x8A949A)};
 // The x-axis always represents the full two-minute window.  Before the
 // history buffer fills, put the available (oldest-to-newest) samples at the
 // end of that window so the latest reading remains at "now".
 const size_t firstPoint = count < kPoints ? kPoints - count : 0;
 for(int s=0;s<4;s++){lv_draw_line_dsc_t line;lv_draw_line_dsc_init(&line);line.color=colors[s];line.width=2;for(size_t i=1;i<count;i++){if(!std::isfinite(values[s][i-1])||!std::isfinite(values[s][i]))continue;int x1=left+(int)((firstPoint+i-1)*w/(kPoints-1)),x2=left+(int)((firstPoint+i)*w/(kPoints-1));lv_point_t p1{(lv_coord_t)x1,(lv_coord_t)yFor(a,values[s][i-1])},p2{(lv_coord_t)x2,(lv_coord_t)yFor(a,values[s][i])};lv_draw_line(ctx,&line,&p1,&p2);}}
}
void kpi(lv_obj_t* parent, int index, lv_color_t color, const char* name) {
 lv_obj_t* item=lv_obj_create(parent);lv_obj_remove_style_all(item);lv_obj_set_size(item,0,LV_SIZE_CONTENT);lv_obj_set_flex_grow(item,1);lv_obj_set_flex_flow(item,LV_FLEX_FLOW_COLUMN);lv_obj_set_flex_align(item,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
 lv_obj_t* label=lv_label_create(item);lv_obj_set_style_text_color(label,color,0);lv_obj_set_style_text_font(label,&lv_font_montserrat_14,0);lv_label_set_text(label,name);
 lv_obj_t* valueRow=lv_obj_create(item);lv_obj_remove_style_all(valueRow);lv_obj_set_size(valueRow,LV_SIZE_CONTENT,LV_SIZE_CONTENT);lv_obj_set_style_pad_column(valueRow,2,0);lv_obj_set_flex_flow(valueRow,LV_FLEX_FLOW_ROW);lv_obj_set_flex_align(valueRow,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_END,LV_FLEX_ALIGN_END);
 kpiValues[index]=lv_label_create(valueRow);lv_obj_set_style_text_font(kpiValues[index],&lv_font_montserrat_28,0);lv_label_set_text(kpiValues[index],"--");
 lv_obj_t* unit=lv_label_create(valueRow);lv_obj_set_style_text_color(unit,ui_theme::mutedText(),0);lv_obj_set_style_text_font(unit,&lv_font_montserrat_14,0);lv_obj_set_style_pad_bottom(unit,3,0);lv_label_set_text(unit,"W");
}
void update(lv_timer_t*) {
 const size_t inCount=sensors::getRecent(sensors::SENSOR_IN,samples[0],kPoints); const size_t outCount=sensors::getRecent(sensors::SENSOR_OUT,samples[1],kPoints); const size_t auxCount=sensors::getRecent(sensors::SENSOR_AUX,samples[2],kPoints); const size_t n=fminf(inCount,fminf(outCount,auxCount)); count=n; float lo=0,hi=0;
 for(size_t i=0;i<n;i++){ values[0][i]=sensors::isCalculationEligible(samples[0][i])?samples[0][i].power:NAN;values[1][i]=sensors::isCalculationEligible(samples[1][i])?samples[1][i].power:NAN;values[2][i]=sensors::isCalculationEligible(samples[2][i])?samples[2][i].power:NAN;values[3][i]=power_flow::balance(values[0][i],values[1][i],values[2][i]);for(int s=0;s<4;s++){if(!std::isfinite(values[s][i]))continue;lo=fminf(lo,values[s][i]);hi=fmaxf(hi,values[s][i]);}}
 step=nice((hi-lo)/6);maximum=ceilf(hi/step)*step;minimum=floorf(lo/step)*step;if(maximum<=minimum){maximum=step;minimum=-step;}for(int s=0;s<4;s++){char b[12];if(n&&std::isfinite(values[s][n-1]))lv_snprintf(b,sizeof(b),"%d",(int)lroundf(values[s][n-1]));else lv_snprintf(b,sizeof(b),"--");lv_label_set_text(kpiValues[s],b);}lv_obj_invalidate(plot);
}
}
void visibleUpdate(lv_timer_t* timer) {
 if (!timer || !timer->user_data || !lv_obj_is_visible(static_cast<lv_obj_t*>(timer->user_data))) return;
 update(timer);
}
lv_obj_t* create(lv_obj_t* parent){lv_obj_t* screen=lv_obj_create(parent);ui_theme::styleScreen(screen,6);lv_obj_set_flex_flow(screen,LV_FLEX_FLOW_COLUMN);lv_obj_set_style_pad_row(screen,4,0);lv_obj_t* row=lv_obj_create(screen);lv_obj_remove_style_all(row);lv_obj_set_size(row,lv_pct(100),LV_SIZE_CONTENT);lv_obj_set_style_pad_all(row,0,0);lv_obj_set_flex_flow(row,LV_FLEX_FLOW_ROW);kpi(row,0,lv_color_hex(0x0000FF),"Solar");kpi(row,1,lv_color_hex(0xFFA500),"Load");kpi(row,2,lv_color_hex(0x00BFFF),"Bat");kpi(row,3,lv_color_hex(0x8A949A),"Balance");plot=lv_obj_create(screen);lv_obj_remove_style_all(plot);lv_obj_set_width(plot,lv_pct(100));lv_obj_set_flex_grow(plot,1);lv_obj_add_event_cb(plot,drawCb,LV_EVENT_DRAW_MAIN,nullptr);lv_timer_create(visibleUpdate,kRefreshMs,screen);return screen;}
}
