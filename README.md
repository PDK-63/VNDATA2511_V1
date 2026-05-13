											
Chức năng 1	- Đẩy dữ liệu nhiệt độ, độ ẩm, tọa độ lên Server

Chức năng 2	- Đẩy tín hiệu InOut lên Server	

Chức năng 4	- Báo động thông qua tin nhắn SMS (05 Số)		

Chức năng 5	- Báo động thông qua cuộc gọi (01 Số hoặc 05 số do người dùng chọn)		

Chức năng 6	- Hệ thống cảnh báo bộ điều khiển quá nhiệt (Sẽ gửi tín hiệu SMS cảnh báo cuối và tự ngắt thiết bị)			

Chức năng 7	"- Vượt ngưỡng nhiệt độ cao, thấp (NTC)
  1. Khi tbi vượt ngưỡng nhiệt độ cao: Báo động qua SMS (05 SĐT), Báo động qua gọi điện (05 SĐT), Còi ON, Output 1 ON.
  2. Khi tbi vượt ngưỡng nhiệt độ thấp: Báo động qua SMS (05 SĐT), Báo động qua gọi điện (05 SĐT), Còi ON, Output 1 ON.
  3. Khi trở lại trạng thái bình thường gửi SMS (05 SĐT): ""Thiet bi ... Nhiet do tro lai binh thuong: ... "",  Còi OFF, Output 1 OFF.

SMS Nhiệt Độ vượt ngưỡng: CANH BAO - ID Thiết bị; NHIET DO VUOT NGUONG CAI DAT (Giới hạn thấp, Giới hạn cao); Nhiệt độ hiện tại; Time.
Ví dụ: ""CANH BAO - Thiet Bi: ...; NHIET DO VUOT NGUONG CAI DAT (-30.00 , -8.00); 16.00; (04h00 21/01/2026).""

SMS Nhiệt độ trở lại bình thường: Thong Bao - ID Thiết bị ; Nhiet Do Tro Lai Binh Thuong(Giới hạn thấp, Giới hạn cao); Nhiệt độ hiện tại; Time.
Ví dụ: ""Thong Bao - Thiet Bi: ...; Nhiet Do Tro Lai Binh Thuong; 10.00(-5.00, 20.00 ); (04h15 - 21/01/2026)."""						
										
Chức năng 8	"- Vượt ngưỡng độ ẩm cao, thấp (SHT)
  1. Khi tbi vượt ngưỡng độ ẩm cao: Báo động qua SMS (05 SĐT), Báo động qua gọi điện (05 SĐT), Còi ON, Output 1 ON.
  2. Khi tbi vượt ngưỡng độ ẩm thấp: Báo động qua SMS (05 SĐT), Báo động qua gọi điện (05 SĐT), Còi ON, Output 1 ON.
  3. Khi trở lại trạng thái bình thường gửi SMS (05 SĐT): ""Thiet bi ... Do am tro lai binh thuong: ... "", Còi OFF, Output 1 OFF.

SMS Độ ẩm vượt ngưỡng: CANH BAO - ID Thiết bị; DO AM VUOT NGUONG CAI DAT (Giới hạn thấp, Giới hạn cao); Độ ẩm hiện tại; Time.
Ví dụ: ""CANH BAO - Thiet Bi: 2510ABC123; DO AM VUOT NGUONG CAT DAT (45% - 80%); 45%; (04h00 21/01/2026).""

SMS Độ ẩm trở lại bình thường: Thong Bao - ID Thiết bị ; Do Am Tro Lai Binh Thuong; Độ ẩm hiện tại; Time.
Ví dụ: ""Thong Bao - Thiet Bi: 2510ABC123; Do Am Tro Lai Binh Thuong; 80%; (04h15 - 21/01/2026)."""						
										
Chức năng 9	"- Đầu vào 02 (Input 02)
  1. Đầu vào 02 có tín hiệu: Báo động qua SMS (05 SĐT), Báo động qua gọi điện (01 SĐT). ""Thiet bi … Canh bao loi 02""
  2. Đầu vào 02 hết lỗi: Báo động qua SMS (05 SĐT), Báo động qua gọi điện (01 SĐT). ""Thiet bi … Het loi 02"""						

Chức năng 10	"- 07h hàng ngày thì gửi tin nhắn đến 01 sđt đầu tiên:  ""Thiet Bi … Dang Hoat Dong"" (nếu để là ""0...9"" thì off chức năng này)

Thong Bao - Thiết bị: ID; Dang Hoat Dong; Nhiệt độ hiện tại (NTC) (Giới hạn Thấp, Cao); Độ ẩm hiện tại (SHT) (Giới hạn Thấp , Cao); Nguồn chính(Co Dien, Mat Dien); Thời gian.
Ví dụ: Thong Bao - 2510ABCD123 (0.9.0.4);Dang Hoat Dong; -30.00 (0-10); 45% (10 - 80) ; Co Dien; 21h00 - 21/01/2026."						
Chức năng 11	"- Báo động khi mất điện
   1. Pin duy trì trong vòng 12h, kể từ khi mất điện, vẫn gửi dữ liệu lên Server thông qua 4G)
   2. Khi mất điện sẽ gửi tin nhắn đến 05 sđt, gọi điện 05 SĐT, Còi ON, Output 1 ON.
   3. Khi có điện trở lại gửi tin nhắn đến 05 SĐT: ""Thiet Bi ... Co Dien Tro Lai"", Còi OFF, Output 1 OFF.

SMS Mât điện: CANH BAO - ID Thiết bị; Nguồn chính (Mất điện, Có Điện); Nhiệt độ hiện tại (NTC); Đô ẩm hiện tại (SHT); Time.
Ví dụ: ""CANH BAO - Thiet Bi: 2510ABC123; MAT DIEN; -16.00; 50%; (04h00 21/01/2026).""

SMS Có điện trở lại: CANH BAO - ID Thiết bị; Nguồn chính (Mất điện, Có Điện); Nhiệt độ hiện tại (NTC); Đô ẩm hiện tại (SHT); Time.
Ví dụ: ""Thong Bao - Thiet Bi: 2510ABC123; Co Dien Tro Lai; -10.00; 50%; (04h00 21/01/2026)."""						
								
Một số lưu ý							
Lưu ý 1	Bộ điều khiển sẽ ưu tiên sử dụng wifi, khi mất tín hiệu wifi sẽ tự động chuyển sang 4G, khi có wifi sẽ tự động kết nối lại		

Lưu ý 3	Nguồn cấp từ 7-30VDC				

Lưu ý 4	Tbi thông báo qua SMS (tên thiết bị sẽ lấy tên theo cài đặt trên web)	

Lưu ý 5	Trong phần cài đặt SĐT cảnh báo SMS, Gọi điện. Nếu để SĐT là "00" thì không gửi đến số đó.	

Lưu ý 6	Ưu tiên sử dụng nguồn chính, khi mất nguồn chính tự động chuyển nguồn phụ, khi có nguồn chính tự động chuyển lại.		

Lưu ý 7	Nguồn chính và phụ sử dụng Jack cắm DC cái						

Lưu ý 8	Thêm công tắc tắt còi (Màu đen) ---> Chuyển sang Reset tắt còi					

Lưu ý 9	Switch để kích hoạt: Nhiệt độ, Độ ẩm, GPS, Spare (03, 01 dự phòng)  Chuyển sang giao diện web		

Lưu ý 10	Chú ý lỗi đứt dây cảm biến						

Lưu ý 12	Việc báo động Call, SMS cần kết thúc trong vòng 2 phút.					

Lưu ý 13	"Khi nguồn chính mất, Thiết bị sử dụng nguồn phụ khi nguồn phụ sắp hết điện thì gửi sms thông báo cuối, và dừng các hoạt động lại.

Cú pháp: ID Thiết bị; Nguon Phu Pin Yeu; Nhiệt độ hiện tại; Độ ẩm hiện tại; Thời gian.
Ví dụ: 2510ABC123 (0.9.0.4); Nguon Phu Pin Yeu; -20.00; 45%; 21h00 - 19/01/2026. "						
			
Next Version	- Hỗ trợ giao tiếp RS-485, Hỗ trợ cổng mạng Lan (RJ45)						
	- Hỗ trợ các loại cảm biến khác bao gồm: Oxy, Gió, PH,…						
			
Các tham số cài đặt trên web							
Các tham số gửi lên	Nhiệt độ, độ ẩm, Tọa độ					Giao diện người dùng	
Thông tin wifi	Tên wifi, mật khẩu					Giao diện người dùng	
Cài đặt Call, SMS	01 SĐT Call; 05 SĐT (SMS hoặc call)					Giao diện người dùng	
Cài đặt ngưỡng cao thấp	Nhiệt độ, độ ẩm.					Giao diện người dùng	
							
							
Cài đặt hiệu chỉnh nhiệt độ, độ ẩm	Nhiệt độ: -55 ~ 99 độ C; Độ ẩm: 0 ~ 99%					Giao diện Admin	
Thời gian nhắc lại cảnh báo	0 là Off Không nhắc lại; Từ 0 ~ 60 phút tương đương với thời gian nhắc lại cảnh báo (SMS, Call)					Giao diện Admin	
Thời gian kích hoạt lại cảnh báo	0 là Off: Cảnh báo xuất hiện ngay sau đó; Từ 0 ~ 60 tương ứng với thời gian trễ kích hoạt cảnh báo					Giao diện Admin	
Kích hoạt chế độ mở rộng	Nhiệt độ, Độ ẩm, GPS					Giao diện Admin, SMS	
		
Thông tin cải tiến							
Nhắc lại cảnh báo	"Nhắc lại cảnh báo ... Phút /1 lần (nếu lỗi vẫn còn xuất hiện),  ở chức năng 7,8,10. (Mặc định cài đặt ban đầu là ko nhắc lại).
Khi có lỗi xuất hiện thiết bị thông báo SMS 05 SĐT, sau đó Báo động Call 05 SĐT.
Báo động nhắc lại 2 lần (Nếu lỗi còn xuất hiện). Mỗi lần cách nhau 20 phút -> Chuyển xuống 10 phút giúp anh nhé!"						
								
Input 1	" Vô hiệu hóa chức năng báo động (Call, SMS, Còi, Output1)
- Mặc định ko đấu gì (OFF) là Bật (Chức năng báo động bình thường)
- Khi tín hiệu On thì: Vô hiệu hóa. Đến khi OFF. (Nếu lỗi mới xuất hiện mà tín hiệu chưa Off thì cũng không được báo động)
- Nếu thiết bị đang báo động mà tín hiệu ON, thì báo động xong sẽ kích hoạt chế độ Vô Hiệu hóa."						
										
Xác nhận lỗi	"Ấn nút xác nhận lỗi ở trên tbi hoặc gửi SMS tới Tbi để xác nhận lỗi
- Nếu tbi đang ko báo động. Ấn nút ko có tác dụng gì
- Nếu đang báo động (SMS 05 số, Call 05 số, Còi On, Output1 On). Người dùng nhấn nút xác nhận lỗi: chạy hết quy trình và hủy lặp lại sau đó Còi OFF, Output 01 Off. Đèn Status nhấp nháy chậm (đỏ) cho đến khi hết lỗi.
- Nếu lỗi mới xuất hiện sẽ báo động trở lại (SMS 05 số, Call 05 số, Còi On, Output1 On). Người dùng cần nhấn nút xác nhận để xác nhận lỗi (Còi OFF, Output 01 Off). Đèn Status nhấp nháy chậm (đỏ)  cho đến khi hết lỗi."							
SMS	"Kiểm tra thông tin thiết bị, cài đặt thông tin wifi, Reset thiết bị.
-  Khi cần kiểm tra người dùng sẽ nhắn tin cho SĐT gắn vào thiết bị theo cú pháp SMS ""TT"" hoặc ""INFOR"".
   Thiết bị sẽ phản hồi nội dung sau: ID Thiết bị (Version); Nhiệt độ hiện tại (NTC) (Giới hạn Thấp, Cao); Độ ẩm hiện tại (SHT) (Giới hạn Thấp , Cao); Nguồn chính(Co Dien, Mat Dien); Thời gian.
   Ví dụ: 2510ABCD123 (0.9.0.4); -30.00 (0-10); 45% (10 - 80) ; Co Dien; 21h00 - 21/01/2026."						
		
Thêm thẻ SD	"Để lưu trữ dữ liệu 300,000 bản ghi (12 tháng) (05 phút log 1 lần)
Nội dung: ID Thiết bị; Nhiệt độ hiện tại (NTC); Độ ẩm hiện tại (SHT); Thời gian hiện tại
Ví dụ: 2510ABC123; -20.00; 45%; 21h00 - 19/01/2026"						
	
Web Local	Cấu hình wifi, Cài đặt 05 SĐT (Chọn SMS hoặc Call), Cài đặt ngưỡng nhiệt độ, độ ẩm Cao, Thấp						
