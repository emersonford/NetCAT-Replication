library(ggplot2)
library(reshape2)
file <- "trial-notchpeak-nanoseconds.csv"
left.x.lim <- 3000
right.x.lim <- 6000
first_read_col <- 1
second_read_col <- 2

data <- read.csv(file)

# hist(data$read1, col=rgb(1, 0, 0, 0.5), xlim=c(4000, 5000), ylim=c(0, 1000), breaks=2500)
# hist(data$read2, col=rgb(0, 0, 1, 0.5), breaks=2500, add=T)

frame <- data.frame(first_read=data[,first_read_col], second_read=data[,second_read_col])
# plot(read1.d, xlim=c(4000, 5000))
# polygon(read1.d, col=rgb(1, 0, 0, 0.5))

ggplot(melt(frame), aes(x = value, fill = variable)) + geom_density(alpha=0.5) + xlim(left.x.lim, right.x.lim) + labs(title=file, x="cycles", fill="read type")
